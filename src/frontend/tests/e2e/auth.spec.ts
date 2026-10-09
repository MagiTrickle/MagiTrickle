import { expect, test } from "@playwright/test";

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
  test("shows ambient branding without interrupting form focus or reduced motion", async ({ page }) => {
    await authPage.goto();

    const logo = page.getByTestId("auth-logo");
    const floatingLogo = logo.locator(".logo-float");
    await expect(logo).toBeVisible();
    await expect(logo.locator("img")).toBeVisible();
    await expect
      .poll(() =>
        logo.locator("img").evaluate((element) =>
          element instanceof HTMLImageElement ? element.naturalWidth : 0,
        ),
      )
      .toBeGreaterThan(0);


    await authPage.loginInput.focus();
    await expect
      .poll(() =>
        floatingLogo.evaluate((element) => getComputedStyle(element).animationPlayState),
      )
      .toBe("paused");

    await page.emulateMedia({ reducedMotion: "reduce" });
    await expect
      .poll(() => floatingLogo.evaluate((element) => getComputedStyle(element).animationName))
      .toBe("none");

    await authPage.loginInput.fill("admin");
    await authPage.passwordInput.fill("secret");
    await expect(authPage.signInButton).toBeEnabled();
  });

});
