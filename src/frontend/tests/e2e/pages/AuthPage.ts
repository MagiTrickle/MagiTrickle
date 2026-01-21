import { type Page, type Locator } from "@playwright/test";

export class AuthPage {
  readonly page: Page;
  readonly loginInput: Locator;
  readonly passwordInput: Locator;
  readonly signInButton: Locator;

  constructor(page: Page) {
    this.page = page;
    this.loginInput = page.locator("#login");
    this.passwordInput = page.locator("#password");
    this.signInButton = page.getByRole("button", { name: "Sign In" });
  }

  async goto() {
    await this.page.goto("/");
  }

  async login(user: string, pass: string) {
    await this.loginInput.fill(user);
    await this.passwordInput.fill(pass);
    await this.signInButton.click();
  }
}
