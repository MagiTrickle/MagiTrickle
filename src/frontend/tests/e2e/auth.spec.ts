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
    await expect(authPage.signInButton).toHaveCSS("color", "rgb(248, 81, 73)");
  });
  test("shows ambient branding without interrupting form focus or reduced motion", async ({
    page,
  }) => {
    await authPage.goto();

    const logo = page.getByTestId("auth-logo");
    const atmosphere = page.locator("canvas");
    await expect(logo).toBeVisible();
    await expect(logo.locator("img")).toBeVisible();
    await expect
      .poll(() =>
        logo
          .locator("img")
          .evaluate((element) => (element instanceof HTMLImageElement ? element.naturalWidth : 0)),
      )
      .toBeGreaterThan(0);

    await expect(atmosphere).toHaveAttribute("data-mode", "animated");
    await authPage.loginInput.focus();
    await expect(atmosphere).toHaveAttribute("data-mode", "paused");

    await page.emulateMedia({ reducedMotion: "reduce" });
    await expect(atmosphere).toHaveAttribute("data-mode", "static");

    await authPage.loginInput.fill("admin");
    await authPage.passwordInput.fill("secret");
    await expect(authPage.signInButton).toBeEnabled();
  });

  test("keeps the original logo and keyboard login usable without WebGL", async ({ page }) => {
    await page.addInitScript(() => {
      const getContext = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function (this: HTMLCanvasElement, type, ...args) {
        if (type === "webgl") return null;
        return Reflect.apply(getContext, this, [type, ...args]);
      } as typeof getContext;
    });
    await page.route("**/auth", async (route) => {
      if (route.request().method() === "POST") {
        expect(route.request().postDataJSON()).toEqual({ login: "admin", password: "secret" });
        await route.fulfill({ json: { token: "fallback-token" } });
      } else {
        await route.fulfill({ json: { enabled: true } });
      }
    });
    await page.route("**/groups?with_rules=true", (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await authPage.goto();
    await expect(page.locator("canvas")).toHaveAttribute("data-mode", "fallback");
    await expect(page.getByTestId("auth-logo").locator("img")).toHaveCSS("opacity", "1");
    await authPage.loginInput.fill("admin");
    await authPage.passwordInput.fill("secret");
    await authPage.passwordInput.press("Enter");
    await expect(page.locator(".group-controls")).toBeVisible();
  });

  test("recovers its scene after context loss without clearing credentials", async ({ page }) => {
    await authPage.goto();
    const atmosphere = page.locator("canvas");
    await expect(atmosphere).toHaveAttribute("data-mode", "animated");
    await authPage.loginInput.fill("admin");
    await authPage.passwordInput.fill("secret");
    await atmosphere.evaluate((canvas: HTMLCanvasElement) => {
      canvas.getContext("webgl")?.getExtension("WEBGL_lose_context")?.loseContext();
    });
    await expect(atmosphere).toHaveAttribute("data-mode", "fallback");
    await expect(page.getByTestId("auth-logo").locator("img")).toHaveCSS("opacity", "1");
    await atmosphere.evaluate((canvas: HTMLCanvasElement) => {
      canvas.getContext("webgl")?.getExtension("WEBGL_lose_context")?.restoreContext();
    });
    await expect(atmosphere).toHaveAttribute("data-mode", "paused");
    await expect(authPage.loginInput).toHaveValue("admin");
    await expect(authPage.passwordInput).toHaveValue("secret");
    await expect(authPage.signInButton).toBeEnabled();
  });
});
