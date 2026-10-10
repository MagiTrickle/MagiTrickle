import { expect, test } from "@playwright/test";

test.describe("Header Settings", () => {
  test.beforeEach(async ({ page }) => {
    // Mock auth disabled to access layout directly
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
    // The header always mounts the updater; keep unrelated UI checks offline.
    await page.route("**/system/update/status", (route) =>
      route.fulfill({ json: { stage: "idle" } }),
    );
    await page.route("**/system/update", (route) =>
      route.fulfill({
        json: {
          installed_version: "1.0.0",
          installed_revision: 1,
          asset_suffix: "test.ipk",
          can_install: true,
          reason: "",
        },
      }),
    );
    await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
    await page.goto("/");
  });

  test("should display version", async ({ page }) => {
    // Version is in .version span.version-text
    const version = page.locator(".version .version-text");
    await expect(version).toBeVisible();
    await expect(version).not.toHaveText("");
  });

  test("version popup opens from keyboard and follows language", async ({ page }) => {
    const trigger = page.getByRole("button", { name: /Software update:/ });
    await expect(trigger).toBeVisible();
    await trigger.focus();
    await page.keyboard.press("Enter");
    const popup = page.getByRole("dialog", { name: "Software update", exact: true });
    await expect(popup).toBeVisible();
    await expect(popup.getByRole("button", { name: "Up to date" })).toBeDisabled();
    await page.keyboard.press("Escape");

    await page.locator(".locale button").click();
    await page.getByRole("button", { name: /Обновление программы:/ }).click();
    await expect(page.getByRole("dialog", { name: "Обновление программы" })).toBeVisible();
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

  test("should open info dialog", async ({ page }) => {
    const infoBtn = page.locator(".info button");
    await expect(infoBtn).toBeVisible();

    await infoBtn.click();

    // Check dialog title
    const dialog = page.locator("[data-dialog-content]");
    await expect(dialog).toBeVisible();
    await expect(dialog.getByText("About", { exact: true })).toBeVisible();

    // Check for some content
    await expect(dialog.locator("text=Official website")).toBeVisible();
    await expect(dialog.locator("text=Bug Tracker")).toBeVisible();

    // Close dialog
    await page.keyboard.press("Escape");
    await expect(dialog).not.toBeVisible();
  });
});

test("logout removes the hovered tooltip with its trigger", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("auth", JSON.stringify({ value: "test-token" }));
  });
  await page.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/auth")) return route.fulfill({ json: { enabled: true } });
    if (path.endsWith("/interfaces")) return route.fulfill({ json: { interfaces: [] } });
    if (path.endsWith("/profiles")) return route.fulfill({ json: { profiles: [] } });
    return route.fulfill({ json: { groups: [] } });
  });
  await page.goto("/");
  const logout = page.locator(".logout button");
  await logout.hover();
  await expect(page.locator("#global-tooltip")).toHaveText("Logout");
  await logout.click();
  await expect(page.locator("#login")).toBeVisible();
  await expect(page.locator("#global-tooltip")).toHaveCount(0);
});
