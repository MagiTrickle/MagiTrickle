import { expect, test } from "@playwright/test";

import type { Subscription } from "../../src/types";

// A real UI flow with a deterministic 50k response, no public upstream dependency.
test("50k subscription: summary, URL-only create, compact save, canonical IDs", async ({
  page,
}) => {
  const subscription: Subscription = {
    id: "aabbccdd",
    name: "Large list",
    url: "https://example.com/large.txt",
    interface: "eth0",
    enable: true,
    priority: 100,
    interval: 86400,
    lastUpdate: 1700000000,
    rules: Array.from({ length: 50000 }, (_, i) => ({
      id: (i + 1).toString(16).padStart(8, "0"),
      rule: `10.${i >> 8}.${i & 255}.0/24`,
      type: "subnet",
      enable: true,
    })),
  };
  let created = false;
  let summarySeen = false;
  const saves: Record<string, any>[] = [];
  await page.route("**/auth", (r) => r.fulfill({ json: { enabled: false } }));
  await page.route("**/groups?with_rules=true", (r) => r.fulfill({ json: { groups: [] } }));
  await page.route("**/interfaces", (r) => r.fulfill({ json: { interfaces: [{ id: "eth0" }] } }));
  await page.route("**/api/v1/subscriptions**", async (route) => {
    const req = route.request();
    const url = new URL(req.url());
    if (url.pathname.endsWith("/rules")) {
      expect(url.searchParams.get("summary")).toBe("true");
      summarySeen = true;
      await route.fulfill({ json: { count: 50000, types: { subnet: 50000 } } });
      return;
    }
    if (req.method() === "POST") {
      expect(url.searchParams.get("fetch")).toBe("true");
      const body = req.postDataJSON();
      expect(body).not.toHaveProperty("rules");
      expect(body).not.toHaveProperty("id");
      expect(body.url).toBe(subscription.url);
      expect(req.postData()!.length).toBeLessThan(1024);
      created = true;
      await route.fulfill({ json: { subscription } });
      return;
    }
    if (req.method() === "PUT") {
      expect(req.postData()!.length).toBeLessThan(1024);
      saves.push(req.postDataJSON().subscriptions[0]);
      await route.fulfill({ json: { status: "ok" } });
      return;
    }
    await route.fulfill({ json: { subscriptions: created ? [subscription] : [] } });
  });
  await page.goto("/");
  await page.getByRole("tab", { name: "Subscriptions" }).click();
  await page.getByRole("button", { name: "Add Subscription", exact: true }).click();
  await page.locator("#sub-url").fill(subscription.url);
  await page.getByRole("button", { name: "Next", exact: true }).click();
  await expect(page.getByText("Found rules: 50000", { exact: true })).toBeVisible();
  expect(summarySeen).toBe(true);
  await page.locator("#sub-name").fill("Large list");
  await page.getByRole("button", { name: "Add", exact: true }).click();
  await expect(page.locator(".subscription-panel")).toHaveCount(1);
  await expect(page.locator(".subscription-rule")).toHaveCount(50);
  await page.locator(".subscription-url-input").fill("https://example.com/renamed.txt");
  await page.locator("#save-subscriptions").click();
  await expect.poll(() => saves.length).toBe(1);
  expect(saves[0].ruleChanges).toEqual([]);
  expect(saves[0]).not.toHaveProperty("rules");
  await page.locator(".subscription-rule").first().getByRole("switch").click();
  await page.locator("#save-subscriptions").click();
  await expect.poll(() => saves.length).toBe(2);
  expect(saves[1].ruleChanges).toEqual([
    {
      id: "00000001",
      rule: "10.0.0.0/24",
      previousType: "subnet",
      previousEnable: true,
      type: "subnet",
      enable: false,
    },
  ]);
});
