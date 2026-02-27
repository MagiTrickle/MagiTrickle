import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Groups Search", () => {
  let groupsPage: GroupsPage;

  test.beforeEach(async ({ page }) => {
    groupsPage = new GroupsPage(page);

    await page.route("**/auth", async (route) => {
      await route.fulfill({ json: { enabled: false } });
    });

    // Mock initial groups with known data
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "1",
              name: "Group Alpha",
              rules: [],
              color: "#000000",
              enable: true,
              interface: "",
            },
            {
              id: "2",
              name: "Group Beta",
              rules: [],
              color: "#000000",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await page.route("**/interfaces", async (route) => {
      await route.fulfill({ json: { interfaces: [] } });
    });

    await groupsPage.goto();
  });

  test("should filter groups by name", async ({ page }) => {
    await expect(page.locator(".group-wrapper")).toHaveCount(2);

    await groupsPage.search("Alpha");
    // Count visible groups only
    await expect(page.locator(".group-wrapper:visible")).toHaveCount(1);

    // Check visible group name
    await expect(page.locator(".group-wrapper:visible input.group-name")).toHaveValue(
      "Group Alpha",
    );
    // Ensure Beta is not visible (checking by value in invisible elements is tricky,
    // but we verified count is 1. We can assume the visible one is Alpha).

    await groupsPage.search("Beta");
    await expect(page.locator(".group-wrapper:visible")).toHaveCount(1);
    await expect(page.locator(".group-wrapper:visible input.group-name")).toHaveValue("Group Beta");
  });

  test("should clear search results when cleared", async ({ page }) => {
    await groupsPage.search("Alpha");
    await expect(page.locator(".group-wrapper:visible")).toHaveCount(1);

    await groupsPage.search("");
    await expect(page.locator(".group-wrapper:visible")).toHaveCount(2);
  });

  test("should filter rules by name", async ({ page }) => {
    // Override mock to have rules
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "1",
              name: "Group A",
              rules: [
                { id: "12345671", name: "Alpha Rule", rule: "a", type: "namespace", enable: true },
                { id: "12345672", name: "Beta Rule", rule: "b", type: "namespace", enable: true },
              ],
              color: "#000000",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await groupsPage.goto();

    await expect(page.locator(".rule")).toHaveCount(2);

    await groupsPage.search("Alpha");
    await expect(page.locator(".rule")).toHaveCount(1);
    await expect(page.locator(".rule .name input").first()).toHaveValue("Alpha Rule");

    await groupsPage.search("Beta");
    await expect(page.locator(".rule")).toHaveCount(1);
    await expect(page.locator(".rule .name input").first()).toHaveValue("Beta Rule");
  });

  test("should filter rules by pattern", async ({ page }) => {
    // Override mock to have rules with distinct patterns
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "1",
              name: "Group A",
              rules: [
                {
                  id: "12345671",
                  name: "Google",
                  rule: "google.com",
                  type: "domain",
                  enable: true,
                },
                {
                  id: "12345672",
                  name: "Local",
                  rule: "192.168.1.1",
                  type: "subnet",
                  enable: true,
                },
              ],
              color: "#000000",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await groupsPage.goto();

    // Search by pattern part
    await groupsPage.search("192");
    await expect(page.locator(".rule")).toHaveCount(1);
    await expect(page.locator(".rule .pattern input").first()).toHaveValue("192.168.1.1");

    // Search by other pattern
    await groupsPage.search("google");
    await expect(page.locator(".rule")).toHaveCount(1);
    await expect(page.locator(".rule .pattern input").first()).toHaveValue("google.com");
  });

  test("should highlight matched text in group name, rule name, and pattern", async ({ page }) => {
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({
        json: {
          groups: [
            {
              id: "g-highlight",
              name: "Zeta Group",
              rules: [
                {
                  id: "r-name",
                  name: "Alpha Rule",
                  rule: "first.example.com",
                  type: "domain",
                  enable: true,
                },
                {
                  id: "r-pattern",
                  name: "Network Rule",
                  rule: "needle.example.com",
                  type: "domain",
                  enable: true,
                },
              ],
              color: "#000000",
              enable: true,
              interface: "",
            },
          ],
        },
      });
    });

    await groupsPage.goto();

    const group = page.locator(".group-wrapper").first();
    const groupNameOverlay = group.locator(".group-name-field .group-name-search-overlay");

    await groupsPage.search("zeta");
    await expect(page.locator(".group-wrapper:visible")).toHaveCount(1);
    await expect(groupNameOverlay.locator("mark")).toHaveCount(1);
    await expect(groupNameOverlay.locator("mark").first()).toHaveText("Zeta");
    await expect(group.locator(".rule .search-highlight-overlay mark")).toHaveCount(0);

    await groupsPage.search("alpha");
    await expect(group.locator(".rule")).toHaveCount(1);
    const nameMatchedRule = group.locator('.rule[data-uuid="r-name"]');
    await expect(nameMatchedRule.locator(".name .search-highlight-overlay mark")).toHaveCount(1);
    await expect(nameMatchedRule.locator(".name .search-highlight-overlay mark").first()).toHaveText(
      "Alpha",
    );
    await expect(nameMatchedRule.locator(".pattern .search-highlight-overlay mark")).toHaveCount(0);
    await expect(groupNameOverlay.locator("mark")).toHaveCount(0);

    await groupsPage.search("needle");
    await expect(group.locator(".rule")).toHaveCount(1);
    const patternMatchedRule = group.locator('.rule[data-uuid="r-pattern"]');
    await expect(patternMatchedRule.locator(".pattern .search-highlight-overlay mark")).toHaveCount(
      1,
    );
    await expect(
      patternMatchedRule.locator(".pattern .search-highlight-overlay mark").first(),
    ).toHaveText("needle");
    await expect(patternMatchedRule.locator(".name .search-highlight-overlay mark")).toHaveCount(0);
    await expect(groupNameOverlay.locator("mark")).toHaveCount(0);
  });
});
