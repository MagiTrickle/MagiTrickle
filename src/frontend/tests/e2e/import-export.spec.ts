import fs from "fs";
import path from "path";
import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Import/Export", () => {
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

  test("should export config", async ({ page }) => {
    // Setup data
    await groupsPage.createGroup();

    // Trigger download
    const downloadPromise = page.waitForEvent("download");
    // Click Export button (3rd button in controls)
    await page.locator(".group-controls-actions button").nth(2).click();

    const download = await downloadPromise;
    expect(download.suggestedFilename()).toBe("config.mtrickle");
  });

  test("should import config", async ({ page }) => {
    // Create dummy config file with valid hex ID (8 chars)
    const configData = {
      groups: [
        {
          id: "12345678",
          name: "Imported Group",
          rules: [],
          color: "#123456",
          enable: true,
          interface: "",
        },
      ],
    };
    const testDir = "tests/e2e/fixtures";
    if (!fs.existsSync(testDir)) fs.mkdirSync(testDir, { recursive: true });
    const filePath = path.join(testDir, "config.mtrickle");
    fs.writeFileSync(filePath, JSON.stringify(configData));

    // Handle file chooser
    const fileChooserPromise = page.waitForEvent("filechooser");
    // Click Import button (2nd button)
    await page.locator(".group-controls-actions button").nth(1).click();

    const fileChooser = await fileChooserPromise;
    await fileChooser.setFiles(filePath);

    // Dialog should appear
    await expect(page.getByRole("dialog")).toBeVisible();

    // Check if the group is listed in the dialog
    await expect(page.getByText("Imported Group")).toBeVisible();

    // Ensure selection by clicking Select All (sometimes selection might default to empty depending on timing)
    await page.getByRole("button", { name: "Select all", exact: true }).click();

    // Click Import inside the dialog
    // The dialog has a footer with Import button
    const dialog = page.getByRole("dialog");
    await dialog.getByRole("button", { name: "Import" }).click();

    // Verify group added
    await expect(page.locator(".group-wrapper")).toHaveCount(1);
    await expect(page.locator("input.group-name")).toHaveValue("Imported Group");
  });
});
