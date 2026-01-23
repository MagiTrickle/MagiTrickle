import { test, expect } from "@playwright/test";
import { GroupsPage } from "./pages/GroupsPage";

test.describe("Accessibility", () => {
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

  test("should focus group name on creation", async ({ page }) => {
    await groupsPage.createGroup();

    // After creation, focus should be on the group name input
    const groupNameInput = page.locator(".group-wrapper").first().locator("input.group-name");
    await expect(groupNameInput).toBeFocused();
  });

  test("should tab through group controls", async ({ page }) => {
    await groupsPage.createGroup();
    const groupNameInput = page.locator(".group-wrapper").first().locator("input.group-name");

    // Focus Name
    await groupNameInput.focus();

    // Tab -> Interface Select (First select in the group)
    await page.keyboard.press("Tab");
    const interfaceTrigger = page
      .locator(".group-wrapper")
      .first()
      .locator("[data-select-trigger]")
      .first();
    await expect(interfaceTrigger).toBeFocused();

    // Tab -> Switch (Enable/Disable)
    await page.keyboard.press("Tab");
    const switchBtn = page.locator(".group-wrapper").first().locator(".enable-group");
    await expect(switchBtn).toBeFocused();

    // Tab -> Delete Group Button
    await page.keyboard.press("Tab");
    // Delete button is inside .group-actions.
    // Order: Select, Switch, Delete, Add, Import, Trigger.
    // Select is separate div. Switch is separate.
    // Then Delete.
    // Selector: .group-actions button that has Delete icon, or just next one.
    // Let's use checking if focused element contains delete icon logic or simpler:
    // Just check if we can reach it.
    // Let's assume we want to verify navigation flow.
    const deleteBtn = page
      .locator(".group-wrapper")
      .first()
      .locator(".group-actions button")
      .nth(2);
    // nth(0) is Select, nth(1) is Switch, nth(2) is Delete.
    await expect(deleteBtn).toBeFocused();
  });
});
