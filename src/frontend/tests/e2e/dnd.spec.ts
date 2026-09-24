import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Drag and Drop", () => {
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

  test("should reorder groups", async ({ page }) => {
    // Create Group A
    await groupsPage.createGroup();
    await groupsPage.setGroupName(0, "Group A");

    // Create Group B (added to top)
    await groupsPage.createGroup();
    await groupsPage.setGroupName(0, "Group B");

    // Initial Order: Group B, Group A.
    await expect(
      groupsPage.page.locator(".group-wrapper").nth(0).locator("input.group-name"),
    ).toHaveValue("Group B");
    await expect(
      groupsPage.page.locator(".group-wrapper").nth(1).locator("input.group-name"),
    ).toHaveValue("Group A");

    // Drag Group B to after Group A
    // Source: Grip of first group (Group B)
    const source = groupsPage.page.locator(".group-wrapper").nth(0).locator(".group-grip");

    // Target: Bottom slot of second group (Group A)
    // Note: Slots might be hidden or 0 height until drag starts?
    // Code says opacity 0, but height 1rem. Pointer events none until drag?
    // css: :global(html[data-dnd-scope="group"]) .group-drop-slot { pointer-events: auto; }
    // So we need to start drag first.

    const sourceBox = await source.boundingBox();

    if (!sourceBox) throw new Error("Source grip not found");

    await page.mouse.move(sourceBox.x + sourceBox.width / 2, sourceBox.y + sourceBox.height / 2);
    await page.mouse.down();

    // Now slots should be active.
    const targetGroup = groupsPage.page.locator(".group-wrapper").nth(1);
    const dropSlot = targetGroup.locator(".group-drop-slot--bottom");
    const targetBox = await dropSlot.boundingBox();

    if (!targetBox) throw new Error("Target slot not found");

    await page.mouse.move(targetBox.x + targetBox.width / 2, targetBox.y + targetBox.height / 2, {
      steps: 10,
    });
    await page.mouse.up();

    // Verify order swapped: Group A, Group B
    await expect(
      groupsPage.page.locator(".group-wrapper").nth(0).locator("input.group-name"),
    ).toHaveValue("Group A");
    await expect(
      groupsPage.page.locator(".group-wrapper").nth(1).locator("input.group-name"),
    ).toHaveValue("Group B");
  });

  test("should reorder rules", async ({ page }) => {
    await groupsPage.createGroup();
    // Default rule is Rule 1
    await groupsPage.setRuleName(0, 0, "Rule 1");

    // Add Rule 2 (added to top)
    await groupsPage.addRuleToGroup(0);
    await groupsPage.setRuleName(0, 0, "Rule 2");

    // Initial: Rule 2, Rule 1
    await expect(page.locator(".rule").nth(0).locator(".name input")).toHaveValue("Rule 2");
    await expect(page.locator(".rule").nth(1).locator(".name input")).toHaveValue("Rule 1");

    // Drag Rule 2 (top) to after Rule 1 (bottom)
    const source = page.locator(".rule").nth(0).locator(".grip");
    const target = page.locator(".rule").nth(1);

    const sourceBox = await source.boundingBox();
    const targetBox = await target.boundingBox();

    if (!sourceBox || !targetBox) throw new Error("Box not found");

    await page.mouse.move(sourceBox.x + sourceBox.width / 2, sourceBox.y + sourceBox.height / 2);
    await page.mouse.down();

    // Move to bottom half of target to trigger "after" drop edge
    await page.mouse.move(
      targetBox.x + targetBox.width / 2,
      targetBox.y + targetBox.height * 0.75,
      { steps: 10 },
    );
    await page.mouse.up();

    // Verify order swapped: Rule 1, Rule 2
    await expect(page.locator(".rule").nth(0).locator(".name input")).toHaveValue("Rule 1");
    await expect(page.locator(".rule").nth(1).locator(".name input")).toHaveValue("Rule 2");
  });
  test("should move a rule onto an empty group header", async ({ page }) => {
    await groupsPage.createGroup();
    await groupsPage.setRuleName(0, 0, "Moving rule");
    await groupsPage.createGroup();
    await groupsPage.deleteRule(0, 0);
    const target = page.locator(".group-wrapper").nth(0);
    await expect(target.locator(".rule")).toHaveCount(0);
    await page
      .locator(".group-wrapper")
      .nth(1)
      .locator(".grip")
      .dragTo(target.locator(".group-header"));
    await expect(target.locator(".rule .name input")).toHaveValue("Moving rule");
    await expect(page.locator(".group-wrapper").nth(1).locator(".rule")).toHaveCount(0);
  });

  test("should insert a rule first when dropped onto a populated header", async ({ page }) => {
    await groupsPage.createGroup();
    await groupsPage.setRuleName(0, 0, "Moving rule");
    await groupsPage.createGroup();
    await groupsPage.setRuleName(0, 0, "Existing rule");
    await page
      .locator(".group-wrapper")
      .nth(1)
      .locator(".grip")
      .dragTo(page.locator(".group-header").nth(0));
    const rules = page.locator(".group-wrapper").nth(0).locator(".rule .name input");
    await expect(rules.nth(0)).toHaveValue("Moving rule");
    await expect(rules.nth(1)).toHaveValue("Existing rule");
  });

  test("should highlight only group slots that change the order", async ({ page }) => {
    for (let i = 0; i < 3; i++) await groupsPage.createGroup();
    const groups = page.locator(".group-wrapper");
    const grip = await groups.nth(1).locator(".group-grip").boundingBox();
    if (!grip) throw new Error("Grip not found");
    await page.mouse.move(grip.x + grip.width / 2, grip.y + grip.height / 2);
    await page.mouse.down();
    const visitSlot = async (index: number, allowed: boolean) => {
      const slot = groups.nth(index).locator(".group-drop-slot--bottom");
      const box = await slot.boundingBox();
      if (!box) throw new Error("Slot not found");
      await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2, { steps: 10 });
      await expect(slot).toHaveAttribute("data-drop", allowed ? "allowed" : "denied");
      await expect(page.locator(".group-drop-slot.dragover")).toHaveCount(allowed ? 1 : 0);
    };
    await visitSlot(2, true);
    await visitSlot(1, false);
    await visitSlot(0, false);
    await page.mouse.up();
  });
});
