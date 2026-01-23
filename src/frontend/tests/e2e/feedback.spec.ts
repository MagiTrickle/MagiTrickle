import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Feedback Components", () => {
  let groupsPage: GroupsPage;

  test.beforeEach(async ({ page }) => {
    groupsPage = new GroupsPage(page);
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
    await groupsPage.goto();
  });

  test("should show toast on success", async ({ page }) => {
    await groupsPage.createGroup();

    await page.route("**/groups?save=true", async (route) => {
      await route.fulfill({ status: 200, json: {} });
    });

    // Make valid
    await groupsPage.setGroupName(0, "G");
    await groupsPage.setRuleName(0, 0, "R");
    await groupsPage.setRulePattern(0, 0, "p");

    await groupsPage.save();

    // Use text locator which is more robust
    const toast = page.getByText("Saved");
    await expect(toast).toBeVisible();
  });

  test("should show overlay on loading", async ({ page }) => {
    await groupsPage.createGroup();
    await groupsPage.setGroupName(0, "G");
    await groupsPage.setRuleName(0, 0, "R");
    await groupsPage.setRulePattern(0, 0, "p");

    // Mock slow save
    await page.route("**/groups?save=true", async (route) => {
      // Wait a bit to ensure overlay appears
      await page.waitForTimeout(500);
      await route.fulfill({ status: 200, json: {} });
    });

    // Click save but don't await immediately
    const savePromise = groupsPage.save();

    // Check overlay visible
    const overlay = page.locator(".overlay");
    await expect(overlay).toBeVisible();

    await savePromise;

    // Check overlay hidden after finish
    await expect(overlay).toBeHidden();
  });
});
