import { expect, test } from "@playwright/test";

import type { Group } from "../../src/types";

test("50k group: compact metadata/rule saves and server IDs survive a second edit", async ({
  page,
}) => {
  const group: Group = {
    id: "aabbccdd",
    name: "Large",
    color: "#ffffff",
    interface: "eth0",
    enable: true,
    rules: Array.from({ length: 50000 }, (_, i) => ({
      id: (i + 1).toString(16).padStart(8, "0"),
      name: "",
      rule: `item${i}.example.com`,
      type: "namespace",
      enable: true,
    })),
  };
  const saves: any[] = [];
  await page.route("**/auth", (r) => r.fulfill({ json: { enabled: false } }));
  await page.route("**/interfaces", (r) => r.fulfill({ json: { interfaces: [{ id: "eth0" }] } }));
  await page.route("**/subscriptions", (r) => r.fulfill({ json: { subscriptions: [] } }));
  await page.route("**/groups?with_rules=true", (r) => r.fulfill({ json: { groups: [group] } }));
  await page.route("**/groups?save=true", async (route) => {
    const incoming = route.request().postDataJSON().groups[0];
    saves.push(incoming);
    expect(route.request().postData()!.length).toBeLessThan(1024);
    expect(incoming).not.toHaveProperty("rules");
    group.name = incoming.name;
    for (const change of incoming.ruleChanges) {
      const rule = group.rules.find((item) => item.id === change.id)!;
      Object.assign(rule, {
        name: change.name,
        type: change.type,
        rule: change.rule,
        enable: change.enable,
      });
    }
    // Demonstrate that the editor uses canonical response IDs, not its request snapshot.
    group.rules[0].id = "ffeeddcc";
    await route.fulfill({ json: { groups: [group] } });
  });
  await page.goto("/");
  await expect(page.locator(".group-wrapper")).toHaveCount(1);
  await page.locator("input.group-name").fill("Renamed");
  await page.locator("#save-changes").click();
  await expect.poll(() => saves.length).toBe(1);
  await expect(page.getByText("Saved", { exact: true })).toBeVisible();
  expect(saves[0].ruleChanges).toEqual([]);
  // Existing groups may be collapsed on initial load.
  if (!(await page.locator(".rule .name input").first().isVisible())) {
    await page.locator(".group-header [data-collapsible-trigger]").first().click();
  }
  await page.locator(".rule .name input").first().fill("Edited rule");
  await page.locator("#save-changes").click();
  await expect.poll(() => saves.length).toBe(2);
  expect(saves[1].ruleChanges).toHaveLength(1);
  expect(saves[1].ruleChanges[0].id).toBe("ffeeddcc");
});
