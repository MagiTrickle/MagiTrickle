import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

const DUPLICATE_PATTERN = "shared.example.com";

function buildLargeGroupRules() {
  return Array.from({ length: 60 }, (_, i) => ({
    id: `900000${i.toString().padStart(2, "0")}`,
    name: `Large Rule ${i}`,
    rule: i === 0 || i === 55 ? DUPLICATE_PATTERN : `unique-${i}.example.com`,
    type: "namespace",
    enable: true,
  }));
}

test.describe("Duplicate Indicators", () => {
  let groupsPage: GroupsPage;

  test.beforeEach(async ({ page }) => {
    groupsPage = new GroupsPage(page);

    await page.route("**/auth", async (route) => {
      await route.fulfill({ json: { enabled: false } });
    });

    await page.route("**/interfaces", async (route) => {
      await route.fulfill({ json: { interfaces: [] } });
    });

    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "g-large",
              name: "Large Group",
              rules: buildLargeGroupRules(),
              color: "#000000",
              enable: true,
              interface: "",
            },
            {
              id: "g-second",
              name: "Second Group",
              rules: [
                {
                  id: "81000001",
                  name: "Second Duplicate",
                  rule: DUPLICATE_PATTERN,
                  type: "namespace",
                  enable: true,
                },
                {
                  id: "81000002",
                  name: "Second Unique",
                  rule: "another-unique.example.com",
                  type: "namespace",
                  enable: true,
                },
              ],
              color: "#111111",
              enable: true,
              interface: "",
            },
            {
              id: "g-clean",
              name: "Clean Group",
              rules: [
                {
                  id: "82000001",
                  name: "Clean Rule",
                  rule: "clean-only.example.com",
                  type: "namespace",
                  enable: true,
                },
              ],
              color: "#222222",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await groupsPage.goto();
  });

  test("should show duplicate markers across groups and paginated rules", async ({ page }) => {
    const groupHeaders = page.locator(".group-header");
    await expect(groupHeaders).toHaveCount(3);

    await expect(groupHeaders.nth(0).locator(".group-duplicate-indicator")).toBeVisible();
    await expect(groupHeaders.nth(1).locator(".group-duplicate-indicator")).toBeVisible();
    await expect(groupHeaders.nth(2).locator(".group-duplicate-indicator")).toHaveCount(0);

    await groupHeaders.nth(0).locator("[data-collapsible-trigger]").click();
    const largeGroup = page.locator(".group-wrapper").nth(0);
    await expect(largeGroup.locator(".rule")).toHaveCount(50);

    await expect(
      largeGroup.locator('.rule[data-group-index="0"][data-index="0"] .duplicate-indicator'),
    ).toBeVisible();
    await expect(
      largeGroup.locator('.rule[data-group-index="0"][data-index="1"] .duplicate-indicator'),
    ).toHaveCount(0);

    await largeGroup.getByTitle("Next Page").click();
    await expect(largeGroup.locator(".rule")).toHaveCount(10);
    await expect(
      largeGroup.locator('.rule[data-group-index="0"][data-index="55"] .duplicate-indicator'),
    ).toBeVisible();

    await groupHeaders.nth(1).locator("[data-collapsible-trigger]").click();
    const secondGroup = page.locator(".group-wrapper").nth(1);
    await expect(
      secondGroup.locator('.rule[data-group-index="1"][data-index="0"] .duplicate-indicator'),
    ).toBeVisible();
  });
});
