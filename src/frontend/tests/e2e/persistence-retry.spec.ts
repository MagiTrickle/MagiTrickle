import { expect, test, type Page } from "@playwright/test";

import type { Group, Subscription } from "../../src/types";

type Kind = "groups" | "subscriptions";
type Failure = "persistence" | "server" | "conflict" | "network";

/** Keep runtime and disk distinct. Apply the same strict optimistic checks as
 * the real handlers (also covered by the C HTTP/YAML regression tests). */
async function editor(page: Page, kind: Kind, failures: Failure[]) {
  const initial: Group & Subscription = {
    id: "aabbccdd",
    name: "Retry",
    color: "#ffffff",
    interface: "eth0",
    enable: true,
    url: "https://example.com/list.txt",
    interval: 86400,
    lastUpdate: 1700000000,
    rules: [{ id: "11223344", name: "Rule", rule: "one.example", type: "domain", enable: true }],
  };
  let live = structuredClone(initial);
  let disk = structuredClone(initial);
  const requests: any[] = [];
  const statuses: number[] = [];
  await page.route("**/api/v1/**", async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname;
    if (path.endsWith("/auth")) return route.fulfill({ json: { enabled: false } });
    if (path.endsWith("/interfaces")) return route.fulfill({ json: { interfaces: ["eth0"] } });
    if (path.endsWith(`/${kind}`) && req.method() === "PUT") {
      const incoming = req.postDataJSON()[kind][0];
      requests.push(incoming);
      const failure = failures.shift();
      if (failure === "network") {
        statuses.push(0);
        return route.abort("failed");
      }
      if (failure === "server" || failure === "conflict") {
        const status = failure === "server" ? 500 : 409;
        statuses.push(status);
        return route.fulfill({ status, json: { error: "not applied" } });
      }
      for (const change of incoming.ruleChanges) {
        const rule = live.rules.find((item) => item.id === change.id);
        const matches =
          rule &&
          (kind === "groups"
            ? Object.entries(change.previous).every(
                ([key, value]) => rule[key as keyof typeof rule] === value,
              )
            : rule.type === change.previousType &&
              rule.enable === change.previousEnable &&
              rule.rule === change.rule);
        if (!matches) {
          statuses.push(409);
          return route.fulfill({ status: 409, json: { error: "stale optimistic baseline" } });
        }
      }
      const staged = structuredClone(live);
      for (const change of incoming.ruleChanges) {
        const rule = staged.rules.find((item) => item.id === change.id)!;
        rule.type = change.type;
        rule.enable = change.enable;
        if (kind === "groups") {
          rule.name = change.name;
          rule.rule = change.rule;
        }
      }
      live = staged;
      if (failure === "persistence") {
        statuses.push(500);
        return route.fulfill({
          status: 500,
          json: {
            error: "failed to save config file; changes are active only in memory",
            code: "PERSISTENCE_FAILED",
            applied: true,
            [kind]: [live],
          },
        });
      }
      disk = structuredClone(live);
      statuses.push(200);
      return route.fulfill({ json: kind === "groups" ? { groups: [live] } : { status: "ok" } });
    }
    if (path.endsWith("/groups"))
      return route.fulfill({ json: { groups: kind === "groups" ? [live] : [] } });
    if (path.endsWith("/subscriptions"))
      return route.fulfill({ json: { subscriptions: kind === "subscriptions" ? [live] : [] } });
    return route.continue();
  });
  await page.goto("/");
  if (kind === "subscriptions") await page.getByRole("tab", { name: "Subscriptions" }).click();
  const panel = page.locator(kind === "groups" ? ".group-wrapper" : ".subscription-panel");
  await expect(panel).toHaveCount(1);
  const toggle = panel
    .locator(kind === "groups" ? ".rule" : ".subscription-rule")
    .first()
    .getByRole("switch");
  if (!(await toggle.isVisible()))
    await panel.locator("[data-collapsible-trigger]").first().click();
  await expect(toggle).toBeVisible();
  const save = page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions");
  const warns = () =>
    page.evaluate(() => {
      const event = new Event("beforeunload", { cancelable: true });
      window.dispatchEvent(event);
      return event.defaultPrevented;
    });
  async function submit(expected: number) {
    const count = statuses.length;
    await save.click();
    await expect.poll(() => statuses.length).toBe(count + 1);
    expect(statuses[count]).toBe(expected);
    if (expected === 200) await expect(save).toHaveClass(/inactive/);
    else await expect(save).not.toHaveClass(/inactive/);
  }
  return {
    toggle,
    save,
    submit,
    warns,
    requests,
    statuses,
    live: () => live,
    disk: () => disk,
    restart: () => {
      live = structuredClone(disk);
    },
  };
}

for (const kind of ["groups", "subscriptions"] as const) {
  test(`${kind}: repeated disk failures remain retryable without replaying edits`, async ({
    page,
  }) => {
    const e = await editor(page, kind, ["persistence", "persistence"]);
    await e.toggle.click();
    await e.submit(500);
    expect(e.live().rules[0].enable).toBe(false);
    expect(e.disk().rules[0].enable).toBe(true);
    await expect.poll(e.warns).toBe(true);
    await e.submit(500);
    expect(e.requests[1].ruleChanges).toEqual([]);
    await expect.poll(e.warns).toBe(true);
    await e.submit(200);
    expect(e.requests[2].ruleChanges).toEqual([]);
    expect(e.disk().rules[0].enable).toBe(false);
    await expect.poll(e.warns).toBe(false);
    e.restart();
    await page.reload();
    if (kind === "subscriptions") await page.getByRole("tab", { name: "Subscriptions" }).click();
    await expect(e.save).toHaveClass(/inactive/);
  });

  test(`${kind}: edits after a disk failure use the applied rule baseline`, async ({ page }) => {
    const e = await editor(page, kind, ["persistence"]);
    await e.toggle.click();
    await e.submit(500);
    await expect.poll(e.warns).toBe(true);
    // Reverting to the disk's old value is still a real edit against runtime.
    await e.toggle.click();
    await e.submit(200);
    expect(e.requests[1].ruleChanges).toHaveLength(1);
    const change = e.requests[1].ruleChanges[0];
    expect(kind === "groups" ? change.previous.enable : change.previousEnable).toBe(false);
    expect(change.enable).toBe(true);
    expect(e.disk().rules[0].enable).toBe(true);
    await expect.poll(e.warns).toBe(false);
  });

  for (const failure of ["server", "conflict", "network"] as const) {
    test(`${kind}: ${failure} errors do not advance the optimistic baseline`, async ({ page }) => {
      const e = await editor(page, kind, [failure]);
      await e.toggle.click();
      await e.submit(failure === "server" ? 500 : failure === "conflict" ? 409 : 0);
      expect(e.live().rules[0].enable).toBe(true);
      await expect.poll(e.warns).toBe(true);
      await e.submit(200);
      expect(e.requests[1].ruleChanges).toEqual(e.requests[0].ruleChanges);
      expect(e.requests[1].ruleChanges).toHaveLength(1);
      expect(e.disk().rules[0].enable).toBe(false);
      await expect.poll(e.warns).toBe(false);
    });
  }
}
