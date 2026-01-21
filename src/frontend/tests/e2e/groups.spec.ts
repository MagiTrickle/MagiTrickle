import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Groups Management", () => {
  let groupsPage: GroupsPage;

  test.beforeEach(async ({ page }) => {
    groupsPage = new GroupsPage(page);

    // Mock Auth to be disabled
    await page.route("**/auth", async (route) => {
      await route.fulfill({ json: { enabled: false } });
    });

    // Mock initial empty groups
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({ json: { groups: [] } });
    });

    // Mock Interfaces fetch (it's called in App.svelte)
    await page.route("**/interfaces", async (route) => {
      await route.fulfill({ json: { interfaces: ["eth0", "wlan0"] } });
    });

    await groupsPage.goto();
  });

  test("should create a new group with a default rule", async ({ page }) => {
    // Expect no groups initially
    await expect(page.locator(".group-wrapper")).toHaveCount(0);
    // The "No groups yet" placeholder should be visible
    await expect(page.getByText("No groups yet")).toBeVisible();

    // Create group
    await groupsPage.createGroup();

    // Expect 1 group
    await expect(page.locator(".group-wrapper")).toHaveCount(1);

    // Check if default rule is added
    await expect(page.locator(".rule")).toHaveCount(1);
  });

  test("should edit group name", async ({ page }) => {
    await groupsPage.createGroup();
    const newName = "My Test Group";
    await groupsPage.setGroupName(0, newName);

    // Verify input value
    const input = (await groupsPage.getGroupHeader(0)).locator("input.group-name");
    await expect(input).toHaveValue(newName);
  });

  test("should add a rule to a group", async ({ page }) => {
    await groupsPage.createGroup();

    // Add another rule
    await groupsPage.addRuleToGroup(0);

    // Expect 2 rules
    await expect(page.locator(".rule")).toHaveCount(2);
  });

  test("should save changes", async ({ page }) => {
    await groupsPage.createGroup();

    // Fill required fields to make the group valid
    await groupsPage.setGroupName(0, "Valid Group");
    await groupsPage.setRuleName(0, 0, "Valid Rule");
    await groupsPage.setRulePattern(0, 0, "domain.com");

    // Wait for validation to update (debounced check might happen)
    // The code does: setTimeout(checkRulesValidityState, 10) on some events,
    // and input events trigger it.
    // Wait for the button to become active
    await expect(groupsPage.saveButton).not.toHaveClass(/inactive/);

    // Mock save request
    let saveRequestReceived = false;
    await page.route("**/groups?save=true", async (route) => {
      saveRequestReceived = true;
      const body = route.request().postDataJSON();
      // Verify body has the group
      expect(body.groups).toHaveLength(1);
      expect(body.groups[0].name).toBe("Valid Group");
      await route.fulfill({ status: 200, json: {} });
    });

    await groupsPage.save();

    expect(saveRequestReceived).toBe(true);

    // Wait for toast or success indication
    await expect(page.getByText("Saved")).toBeVisible();
    expect(saveRequestReceived).toBe(true);
  });

  test("should delete a rule", async ({ page }) => {
    await groupsPage.createGroup();
    // Initially 1 rule (default)
    await groupsPage.addRuleToGroup(0);
    // Now 2 rules
    await expect(page.locator(".rule")).toHaveCount(2);

    await groupsPage.deleteRule(0, 0); // Delete first rule
    await expect(page.locator(".rule")).toHaveCount(1);
  });

  test("should delete a group", async ({ page }) => {
    await groupsPage.createGroup();
    await expect(page.locator(".group-wrapper")).toHaveCount(1);

    // Handle confirmation dialog
    page.on("dialog", (dialog) => dialog.accept());

    await groupsPage.deleteGroup(0);
    await expect(page.locator(".group-wrapper")).toHaveCount(0);
  });

  test("should sort rules", async ({ page }) => {
    await groupsPage.createGroup();
    // 1st rule
    await groupsPage.setRuleName(0, 0, "Rule B");

    // 2nd rule
    await groupsPage.addRuleToGroup(0);
    await groupsPage.setRuleName(0, 0, "Rule A");

    // Initial order: Rule A (top), Rule B (bottom) because addRuleToGroup adds to start (unshift).
    const rules = page.locator(".rule");
    await expect(rules.nth(0).locator(".name input")).toHaveValue("Rule A");
    await expect(rules.nth(1).locator(".name input")).toHaveValue("Rule B");

    // Click Sort by Name (header)
    const nameHeader = page
      .locator(".group-rules-header-column.clickable")
      .filter({ hasText: "Name" });
    await nameHeader.click();

    // 1st click: Ascending -> Rule A, Rule B.
    await expect(rules.nth(0).locator(".name input")).toHaveValue("Rule A");
    await expect(rules.nth(1).locator(".name input")).toHaveValue("Rule B");

    // 2nd click: Descending -> Rule B, Rule A.
    await nameHeader.click();
    await expect(rules.nth(0).locator(".name input")).toHaveValue("Rule B");
    await expect(rules.nth(1).locator(".name input")).toHaveValue("Rule A");
  });

  test("should toggle group enabled state", async ({ page }) => {
    await groupsPage.createGroup();
    const groupHeader = await groupsPage.getGroupHeader(0);
    const toggle = groupHeader.locator(".enable-group");

    // Initial state: enabled (checked)
    await expect(toggle).toHaveAttribute("aria-checked", "true");

    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-checked", "false");
  });

  test("should collapse and expand group", async ({ page }) => {
    await groupsPage.createGroup();
    // Default is expanded. Content visible.
    await expect(page.locator(".group-rules")).toBeVisible();

    const groupHeader = await groupsPage.getGroupHeader(0);
    const collapseTrigger = groupHeader.locator("[data-collapsible-trigger]");

    // Collapse
    await collapseTrigger.click();
    await expect(page.locator(".group-rules")).toBeHidden();

    // Expand
    await collapseTrigger.click();
    await expect(page.locator(".group-rules")).toBeVisible();
  });

  test("should paginate rules", async ({ page }) => {
    // Generate 60 rules
    const rules = Array.from({ length: 60 }, (_, i) => ({
      id: (10000000 + i).toString(), // 8 digit decimal string is mostly valid? Hex is safer.
      // 10000000 is 8 chars.
      name: `Rule ${i}`,
      rule: `rule${i}`,
      type: "namespace",
      enable: true,
    }));

    // Override mock
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "12345678",
              name: "Large Group",
              rules: rules,
              color: "#000000",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await groupsPage.goto(); // Reload to get new mock

    // Check total count header
    await expect(page.locator(".group-rules-header-column.total")).toHaveText("#60");

    // Ensure group is expanded (fetched groups are closed by default)
    const groupRules = page.locator(".group-rules");
    if (!(await groupRules.isVisible())) {
      await page.locator("[data-collapsible-trigger]").first().click();
    }
    await expect(groupRules).toBeVisible();

    // Check visible rules (first page 50)
    await expect(page.locator(".rule")).toHaveCount(50);

    // Check pagination controls
    // Button uses title attribute "Next Page"
    await expect(page.getByTitle("Next Page")).toBeVisible();

    // Go to next page
    await page.getByTitle("Next Page").click();

    // Check remaining 10
    await expect(page.locator(".rule")).toHaveCount(10);
  });

  test("should toggle rule enabled state", async ({ page }) => {
    await groupsPage.createGroup();
    // Use the default rule
    const rule = await groupsPage.getRule(0, 0);
    // Switch in RuleRow is inside .actions -> Tooltip -> Switch
    // Switch renders button[role="switch"]
    const toggle = rule.locator('.actions button[role="switch"]');

    // Initial state: enabled (checked)
    await expect(toggle).toHaveAttribute("aria-checked", "true");

    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-checked", "false");
  });

  test("should change group color", async ({ page }) => {
    await groupsPage.createGroup();
    const groupHeader = await groupsPage.getGroupHeader(0);
    const colorInput = groupHeader.locator('input[type="color"]');
    // color input is often hidden/0-size, force fill
    await colorInput.fill("#ff0000", { force: true });
    await expect(colorInput).toHaveValue("#ff0000");
  });
});
