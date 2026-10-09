import { expect, test, type Page } from "@playwright/test";

import type { Group, Subscription } from "../../src/types";

type Kind = "groups" | "subscriptions";
type Item = Omit<Group & Subscription, "priority"> & { priority?: number };
type Failure = "server" | "persistence";

async function editor(
  page: Page,
  kind: Kind,
  priority?: number,
  failures: Failure[] = [],
  profile?: string,
  rules: Group["rules"] = [],
  extraGroupCount = 0,
) {
  let live: Item = {
    id: "aabbccdd",
    name: "Priority test",
    interface: "eth0",
    color: "#ffffff",
    enable: true,
    priority,
    profile,
    url: "https://example.com/list.txt",
    interval: 86400,
    lastUpdate: 1700000000,
    rules,
  };
  const extraGroups = Array.from({ length: extraGroupCount }, (_, i) => ({
    ...structuredClone(live),
    id: (i + 100).toString(16).padStart(8, "0"),
    name: `Group ${i + 2}`,
    rules: [],
  }));
  let disk = structuredClone(live);
  const requests: any[] = [];
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (path.endsWith("/auth")) return route.fulfill({ json: { enabled: false } });
    if (path.endsWith("/interfaces"))
      return route.fulfill({ json: { interfaces: [{ id: "eth0" }, { id: "eth1" }] } });
    if (path.endsWith("/profiles"))
      return route.fulfill({
        json: {
          profiles: profile
            ? [
                {
                  id: profile,
                  name: "Shared VPN",
                  interfaces: ["eth0"],
                  on_unavailable: "blackhole",
                },
              ]
            : [],
        },
      });
    if (path.endsWith(`/${kind}`) && request.method() === "PUT") {
      const incoming = request.postDataJSON()[kind][0];
      requests.push(incoming);
      const failure = failures.shift();
      if (failure === "server")
        return route.fulfill({ status: 500, json: { error: "not applied" } });
      const { ruleChanges: _, ...metadata } = incoming;
      live = { ...live, ...metadata, profile: metadata.profile };
      if (failure === "persistence") {
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
      return route.fulfill({
        json: kind === "groups" ? { groups: [live, ...extraGroups] } : { status: "ok" },
      });
    }
    if (path.endsWith("/sync")) {
      live.lastUpdate = (live.lastUpdate ?? 0) + 60;
      return route.fulfill({ json: { rules: live.rules, lastUpdate: live.lastUpdate } });
    }
    if (path.endsWith("/groups"))
      return route.fulfill({ json: { groups: kind === "groups" ? [live, ...extraGroups] : [] } });
    if (path.endsWith("/subscriptions"))
      return route.fulfill({ json: { subscriptions: kind === "subscriptions" ? [live] : [] } });
    return route.continue();
  });
  await page.goto("/");
  if (kind === "subscriptions") await page.getByRole("tab", { name: "Subscriptions" }).click();
  const triggers = page.locator(".priority-trigger");
  const trigger = triggers.first();
  const input = page.getByRole("spinbutton");
  const save = page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions");
  await expect(triggers).toHaveCount(1 + extraGroupCount);
  return {
    trigger,
    input,
    save,
    requests,
    disk: () => disk,
    live: () => live,
    async open() {
      await trigger.click();
      await expect(input).toBeVisible();
    },
    async reload() {
      live = structuredClone(disk);
      await page.reload();
      if (kind === "subscriptions") await page.getByRole("tab", { name: "Subscriptions" }).click();
    },
  };
}

for (const kind of ["groups", "subscriptions"] as const) {
  test(`${kind}: bounded priority saves preserve a profile and survive switching to a direct route`, async ({
    page,
  }) => {
    const e = await editor(page, kind, undefined, [], "shared");
    const defaultPriority = kind === "groups" ? "300" : "100";
    await expect(e.trigger).toHaveText(defaultPriority);
    await expect(e.save).toHaveClass(/inactive/);
    await e.open();
    await expect(e.input).toHaveValue(defaultPriority);
    await expect(e.input).toHaveAttribute("min", "1");
    await expect(e.input).toHaveAttribute("max", "1000");
    await expect(e.input).toHaveAttribute("step", "1");
    for (const invalid of ["0", "-1", "1001", "1.5"]) {
      await e.input.fill(invalid);
      await expect(e.input).toHaveValue(defaultPriority);
      await expect(e.save).toHaveClass(/inactive/);
    }
    await e.input.fill("");
    await e.input.press("Tab");
    await expect(e.input).toHaveValue(defaultPriority);
    await e.input.fill("1");
    await expect(page.getByRole("button", { name: "Decrease priority" })).toBeDisabled();
    await page.getByRole("button", { name: "Increase priority" }).click();
    await expect(e.input).toHaveValue("2");
    await e.input.fill("1000");
    await expect(page.getByRole("button", { name: "Increase priority" })).toBeDisabled();
    await page.getByRole("button", { name: "Decrease priority" }).click();
    await expect(e.input).toHaveValue("999");
    await e.input.fill("750");
    await e.input.fill("1001");
    await expect(e.input).toHaveValue("750");
    await page.keyboard.press("Escape");
    await e.save.click();
    await expect(e.save).toHaveClass(/inactive/);
    expect(e.requests).toHaveLength(1);
    expect(e.requests[0].priority).toBe(750);
    expect(e.requests[0].profile).toBe("shared");
    expect(e.requests[0].ruleChanges).toEqual([]);
    expect(e.requests[0]).not.toHaveProperty("rules");
    await e.reload();
    await expect(e.trigger).toHaveText("750");
    await expect(e.save).toHaveClass(/inactive/);
    const routeSelect = page
      .locator(kind === "groups" ? ".group-header" : ".subscription-actions")
      .getByRole("button", { name: "Route", exact: true });
    await expect(routeSelect).toHaveText("Shared VPN");
    await routeSelect.click();
    await page.getByRole("option", { name: "eth1", exact: true }).click();
    const saved = page.waitForResponse(
      (response) =>
        response.request().method() === "PUT" &&
        new URL(response.url()).pathname.endsWith(`/${kind}`),
    );
    await e.save.click();
    await saved;
    await expect(e.save).toBeDisabled();
    expect(e.requests).toHaveLength(2);
    expect(e.requests[1].priority).toBe(750);
    expect(e.requests[1]).not.toHaveProperty("profile");
    expect(e.requests[1].interface).toBe("eth1");
    expect(e.requests[1].ruleChanges).toEqual([]);
    expect(e.disk().profile).toBeUndefined();
    await e.reload();
    await expect(e.trigger).toHaveText("750");
    await expect(routeSelect).toHaveText("eth1");
  });

  test(`${kind}: priority survives rejected saves and post-apply persistence retries`, async ({
    page,
  }) => {
    const e = await editor(page, kind, 450, ["server", "persistence"]);
    await expect(e.trigger).toHaveText("450");
    await e.open();
    await e.input.fill("850");
    await page.keyboard.press("Escape");
    for (let attempt = 1; attempt <= 3; attempt++) {
      await e.save.click();
      await expect.poll(() => e.requests.length).toBe(attempt);
      if (attempt < 3) await expect(e.save).not.toHaveClass(/inactive/);
      else await expect(e.save).toHaveClass(/inactive/);
      await expect(e.trigger).toHaveText("850");
      expect(e.disk().priority).toBe(attempt < 3 ? 450 : 850);
      expect(e.live().priority).toBe(attempt === 1 ? 450 : 850);
    }
    expect(e.requests.every((request) => request.priority === 850)).toBe(true);
    await e.reload();
    await expect(e.trigger).toHaveText("850");
  });
}

test("syncing subscription rules preserves its unsaved priority", async ({ page }) => {
  const e = await editor(page, "subscriptions", 200);
  await e.open();
  await e.input.fill("600");
  await page.keyboard.press("Escape");
  await page.locator('[data-value="Sync Subscription"] button').click();
  await expect(page.getByText("Synced", { exact: true })).toBeVisible();
  await expect(e.trigger).toHaveText("600");
  await expect(e.save).not.toHaveClass(/inactive/);
  await e.save.click();
  await expect(e.save).toHaveClass(/inactive/);
  expect(e.requests[0].priority).toBe(600);
});

test("a new group starts with priority 300", async ({ page }) => {
  await editor(page, "groups", 450);
  await page.locator('[data-value="Add Group"] button').click();
  await expect(page.locator(".priority-trigger")).toHaveCount(2);
  await expect(page.locator(".priority-trigger").first()).toHaveText("300");
});

async function scrollPosition(page: Page) {
  return page.evaluate(async () => {
    // Let deferred autofocus and layout updates finish before checking the viewport.
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return { window: window.scrollY, body: document.body.scrollTop };
  });
}

for (const width of [1280, 390]) {
  test(`long group keeps scroll when editing priority and saving at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const rules: Group["rules"] = Array.from({ length: 40 }, (_, i) => ({
      id: (i + 1).toString(16).padStart(8, "0"),
      name: "",
      rule: `item${i}.example.com`,
      type: "namespace",
      enable: true,
    }));
    const e = await editor(page, "groups", 300, [], undefined, rules, 20);
    await expect(page.locator(".rule")).toHaveCount(40);
    await page.locator('[data-value="Expand Group"] button').first().click();
    await expect(page.locator(".rule").last()).toBeVisible();
    await expect
      .poll(() => page.evaluate(() => document.documentElement.scrollHeight))
      .toBeGreaterThan(1500);
    await page.evaluate(() => window.scrollTo(0, 60));
    const initialScroll = await scrollPosition(page);
    const box = await e.trigger.boundingBox();
    expect(box).not.toBeNull();
    // Use the visible priority strip without Playwright scrolling to another element.
    await page.mouse.click(box!.x + box!.width / 2, box!.y + 20);
    await expect(e.input).toBeVisible();
    expect(await scrollPosition(page)).toEqual(initialScroll);
    await e.input.fill("450");
    await page.mouse.click(box!.x + box!.width / 2, box!.y + 20);
    await expect(e.input).not.toBeVisible();
    await expect(e.trigger).toBeFocused();
    expect(await scrollPosition(page)).toEqual(initialScroll);
    await e.save.click();
    await expect(e.save).toBeDisabled();
    await expect(page.locator(".overlay")).not.toBeVisible();
    expect(e.requests).toHaveLength(1);
    expect(await scrollPosition(page)).toEqual(initialScroll);
    await page.locator(".group-name").first().fill("Renamed");
    const lowerInput = page.locator(".rule .pattern-input").last();
    await lowerInput.focus();
    await expect(lowerInput).toBeInViewport();
    await page.evaluate(() => window.scrollTo(0, 60));
    // Saving must also preserve the viewport when focus is below it.
    await page.keyboard.press("Control+s");
    await expect(e.save).toBeDisabled();
    await expect(page.locator(".overlay")).not.toBeVisible();
    expect(e.requests).toHaveLength(2);
    expect(e.disk().name).toBe("Renamed");
    await expect(lowerInput).toBeFocused();
    expect(await scrollPosition(page)).toEqual(initialScroll);
  });
}
