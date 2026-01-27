import { expect, test } from "@playwright/test";

test.describe("Header Settings", () => {
  test.beforeEach(async ({ page }) => {
    // Mock auth disabled to access layout directly
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
    await page.goto("/");
  });

  test("should display version", async ({ page }) => {
    // Version is in .version span.version-text
    const version = page.locator(".version .version-text");
    await expect(version).toBeVisible();
    expect(version.textContent.length).toBeGreaterThan(0);
  });

  test("should rotate locale", async ({ page }) => {
    const localeBtn = page.locator(".locale button");

    // Get initial text (flag)
    const initialText = await localeBtn.textContent();

    // Click to rotate
    await localeBtn.click();

    // Verify text changed
    await expect(localeBtn).not.toHaveText(initialText || "");

    // Click again to rotate back (assuming 2 locales, or just rotate more)
    await localeBtn.click();
  });
});
