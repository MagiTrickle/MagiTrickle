import { test, expect } from "@playwright/test";
import { AuthPage } from "./pages/AuthPage";

test.describe("Authentication", () => {
  let authPage: AuthPage;

  test.beforeEach(async ({ page }) => {
    authPage = new AuthPage(page);
    // Mock Auth enabled
    await page.route("**/auth", async (route) => {
      if (route.request().method() === "GET") {
        await route.fulfill({ json: { enabled: true } });
      } else {
        await route.continue();
      }
    });
    // Mock Interfaces
    await page.route("**/interfaces", async (route) => {
      await route.fulfill({ json: { interfaces: [] } });
    });
  });

  test("should display login form", async ({ page }) => {
    await authPage.goto();
    await expect(authPage.loginInput).toBeVisible();
    await expect(authPage.passwordInput).toBeVisible();
  });

  test("should login successfully", async ({ page }) => {
    // Mock login success
    await page.route("**/auth", async (route) => {
      if (route.request().method() === "POST") {
        await route.fulfill({ json: { token: "fake-token" } });
      } else {
        await route.fulfill({ json: { enabled: true } });
      }
    });
    // Mock Groups request (happens after login)
    await page.route("**/groups?with_rules=true", async (route) => {
      await route.fulfill({ json: { groups: [] } });
    });

    await authPage.goto();
    await authPage.login("admin", "admin");

    // Expect redirection to AppLayout (check for group controls or something)
    await expect(page.locator(".group-controls")).toBeVisible();
  });

  test("should show error on failure", async ({ page }) => {
    // Mock login failure
    await page.route("**/auth", async (route) => {
      if (route.request().method() === "POST") {
        await route.fulfill({ status: 401 });
      } else {
        await route.fulfill({ json: { enabled: true } });
      }
    });

    await authPage.goto();
    await authPage.login("admin", "wrong");

    // Expect error indication
    // Note: Toast might not be visible if Toast component is not in AuthPage, checking button class.
    await expect(authPage.signInButton).toHaveClass(/fail/);
  });
});
