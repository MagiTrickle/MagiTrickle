import { expect, test, type Page } from "@playwright/test";

import type { Profile } from "../../src/types";

const keenetic = process.env.VITE_ENTWARE_KN === "1";
type Settings = { enabled: boolean; policy_id: string; mode: "include" | "exclude" };
async function backend(page: Page, enabled = false) {
  const state = {
    settings: { enabled, policy_id: enabled ? "Policy0" : "", mode: "include" } as Settings,
    policies: [
      { id: "Policy0", name: "VPN", available: true },
      { id: "Policy1", name: "VPN", available: true },
    ],
    profiles: [] as Profile[],
    profileWrites: 0,
    writes: 0,
    listReads: 0,
    settingsReads: 0,
    listFailure: false,
    diskFailure: false,
  };
  await page.route("**/api/v1/**", async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace("/api/v1", "");
    if (req.method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
      return;
    }
    if (path === "/auth") {
      await route.fulfill({ json: { enabled: false } });
      return;
    }
    if (path === "/keenetic/policy") {
      state.settingsReads++;
      if (req.method() === "PUT") {
        state.writes++;
        state.settings = req.postDataJSON();
      }
      const value = {
        settings: state.settings,
        runtime: { state: state.settings.enabled ? "ready" : "disabled" },
      };
      await route.fulfill({
        status: req.method() === "PUT" && state.diskFailure ? 500 : 200,
        json:
          req.method() === "PUT" && state.diskFailure
            ? { ...value, code: "PERSISTENCE_FAILED", applied: true, error: "Disk is full" }
            : value,
      });
      return;
    }
    if (path === "/keenetic/policies") {
      state.listReads++;
      await route.fulfill({
        status: state.listFailure ? 502 : 200,
        json: state.listFailure ? { error: "RCI is unavailable" } : { policies: state.policies },
      });
      return;
    }
    if (path === "/profiles") {
      if (req.method() === "PUT") {
        state.profileWrites++;
        state.profiles = req.postDataJSON().profiles;
      }
      await route.fulfill({ json: { profiles: state.profiles } });
      return;
    }
    if (path === "/groups") {
      await route.fulfill({ json: { groups: [] } });
      return;
    }
    if (path === "/subscriptions") {
      await route.fulfill({ json: { subscriptions: [] } });
      return;
    }
    if (path === "/system/interfaces") {
      await route.fulfill({ json: { interfaces: [{ id: "blackhole" }, { id: "tun0" }] } });
      return;
    }
    await route.fulfill({ json: { version: "test" } });
  });
  return state;
}
async function openSettings(page: Page) {
  await page.goto("/");
  if ((page.viewportSize()?.width ?? 1280) <= 700)
    await page.getByRole("button", { name: "Open menu", exact: true }).click();
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
}
const panel = (page: Page) => page.locator(".policy-settings");
const save = (page: Page) => page.locator("#save-settings");

test("generic UI has no policy feature or policy requests", async ({ page }) => {
  test.skip(keenetic, "generic build only");
  const state = await backend(page);
  await openSettings(page);
  await expect(panel(page)).toHaveCount(0);
  expect(state.settingsReads).toBe(0);
  expect(state.listReads).toBe(0);
  await expect(page.locator(".settings-page .page-controls")).toHaveCount(1);
  await expect(save(page)).toHaveCount(1);
});

test.describe("Keenetic policy settings", () => {
  test.skip(!keenetic, "Keenetic-only bundle");
  for (const width of [1280, 390]) {
    test(`mt default follows the policy list and saves its ID (${width}px)`, async ({ page }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      const state = await backend(page);
      await openSettings(page);
      await panel(page)
        .getByRole("switch", { name: "Use policy based filter", exact: true })
        .click();
      const picker = panel(page).getByRole("button", { name: "Router policy", exact: true });
      await expect(picker).toHaveText("mt");
      await expect(save(page)).toBeDisabled();
      await expect(save(page)).not.toHaveClass(/inactive/);
      await expect(
        panel(page).getByText('Create the "mt" policy in Keenetic, then reload this page.'),
      ).toBeVisible();
      await expect(panel(page).getByRole("button", { name: "Refresh policies" })).toHaveCount(0);
      await picker.click();
      await expect(page.getByRole("option", { name: "mt", exact: true })).toBeVisible();
      await page.keyboard.press("Escape");
      state.policies.push({ id: "Policy42", name: "mt", available: true });
      await page.reload();
      await panel(page)
        .getByRole("switch", { name: "Use policy based filter", exact: true })
        .click();
      await expect(picker).toHaveText("mt");
      await expect(save(page)).toBeEnabled();
      const mode = panel(page).getByRole("switch", { name: "Processing mode", exact: true });
      await expect(
        panel(page).getByText("Include: MT processes only devices in the selected policy", {
          exact: true,
        }),
      ).toBeVisible();
      await mode.click();
      await expect(
        panel(page).getByText(
          "Exclude: MT processes all devices except those in the selected policy",
          { exact: true },
        ),
      ).toBeVisible();
      await mode.click();
      await page.locator(".locale button").click();
      await expect(
        panel(page).getByText("Include: MT обрабатывает только устройства выбранной политики", {
          exact: true,
        }),
      ).toBeVisible();
      await page.locator(".locale button").click();
      expect(state.writes).toBe(0);
      await save(page).click();
      await expect(save(page)).toBeDisabled();
      expect(state.settings).toEqual({ enabled: true, policy_id: "Policy42", mode: "include" });
      await page.reload();
      await expect(picker).toHaveText("mt");
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
        width,
      );
    });
  }
  test("off by default, explicit save, include/exclude and reload", async ({ page }) => {
    const state = await backend(page);
    await openSettings(page);
    const toggle = panel(page).getByRole("switch", {
      name: "Use policy based filter",
      exact: true,
    });
    await expect(toggle).not.toBeChecked();
    await expect.poll(() => state.listReads).toBe(1);
    await toggle.click();
    await expect(panel(page).getByRole("switch")).toHaveCount(2);
    await expect(save(page)).toBeDisabled();
    await panel(page).getByRole("button", { name: "Router policy", exact: true }).click();
    await page.getByRole("option", { name: "VPN", exact: true }).nth(0).click();
    expect(state.writes).toBe(0);
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    expect(state.settings).toEqual({ enabled: true, policy_id: "Policy0", mode: "include" });
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    await save(page).click();
    await expect.poll(() => state.settings.mode).toBe("exclude");
    await page.reload();
    await expect(
      panel(page).getByRole("switch", {
        name: "Processing mode",
        exact: true,
      }),
    ).toBeChecked();
    await expect(toggle).toBeChecked();
  });
  test("missing policy is retained; RCI failure never blocks disabling", async ({ page }) => {
    const state = await backend(page, true);
    state.policies = [];
    await openSettings(page);
    await expect(
      panel(page).getByRole("button", { name: "Router policy", exact: true }),
    ).toContainText("Policy0");
    await expect(save(page)).toBeDisabled();
    expect(state.settings.policy_id).toBe("Policy0");
    state.listFailure = true;
    await page.reload();
    await expect.poll(() => state.listReads).toBe(2);
    await panel(page).getByRole("switch", { name: "Use policy based filter", exact: true }).click();
    await expect(save(page)).toBeEnabled();
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    expect(state.settings.enabled).toBe(false);
    expect(state.settings.policy_id).toBe("Policy0");
  });
  test("disk failure acknowledges memory settings and keeps Save available for retry", async ({
    page,
  }) => {
    const state = await backend(page, true);
    await openSettings(page);
    await expect(
      panel(page).getByRole("switch", { name: "Use policy based filter", exact: true }),
    ).toBeChecked();
    await panel(page).getByRole("switch", { name: "Use policy based filter", exact: true }).click();
    state.diskFailure = true;
    await save(page).click();
    await expect(
      panel(page).getByText("Changes are applied but not saved to disk. Retry saving.", {
        exact: true,
      }),
    ).toBeVisible();
    await expect(save(page)).toBeEnabled();
    expect(state.settings.enabled).toBe(false);
    state.diskFailure = false;
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    await expect(panel(page).getByRole("alert")).toHaveCount(0);
    expect(state.writes).toBe(2);
  });
  test("status polling preserves unsaved mode and ID", async ({ page }) => {
    const state = await backend(page, true);
    await openSettings(page);
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    await panel(page).getByRole("button", { name: "Router policy", exact: true }).click();
    await page.getByRole("option", { name: "VPN", exact: true }).nth(1).click();
    await expect.poll(() => state.settingsReads, { timeout: 7000 }).toBeGreaterThan(1);
    await expect(
      panel(page).getByRole("switch", {
        name: "Processing mode",
        exact: true,
      }),
    ).toBeChecked();
    await expect(
      panel(page).getByRole("button", { name: "Router policy", exact: true }),
    ).toContainText("VPN");
    await save(page).click();
    await expect.poll(() => state.settings.policy_id).toBe("Policy1");
    expect(state.settings.mode).toBe("exclude");
  });
  test("narrow viewport remains usable without horizontal overflow", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await backend(page, true);
    await openSettings(page);
    await expect(
      panel(page).getByRole("switch", { name: "Use policy based filter", exact: true }),
    ).toBeChecked();
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    await expect(save(page)).toBeEnabled();
    await expect(save(page)).toBeInViewport();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      390,
    );
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      390,
    );
  });
});

test.describe("Shared settings toolbar", () => {
  test.skip(!keenetic, "Keenetic-only editor; generic keeps a single section");
  for (const width of [1280, 390]) {
    test(`scroll changes the heading and Add action, not drafts or the Save node (${width}px)`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: 900 });
      const state = await backend(page, true);
      state.profiles = Array.from({ length: 8 }, (_, i) => ({
        id: `profile${i}`,
        name: `VPN ${i + 1}`,
        interfaces: ["tun0"],
        on_unavailable: "blackhole",
      }));
      await openSettings(page);
      const heading = page.locator(".settings-toolbar").getByRole("heading", { level: 2 });
      const name = page.getByPlaceholder("Profile name", { exact: true }).first();
      await name.fill("Unsaved profile name");
      await expect(save(page)).toBeEnabled();
      const button = await save(page).elementHandle();
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      // Reaching the bottom must preserve a still-visible section's title and Add.
      await expect(heading).toHaveAccessibleName("Interface priority profiles");
      await expect(panel(page).getByRole("heading", { name: "Keenetic policy" })).toBeInViewport();
      await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
      // The compact policy section needs a shorter viewport to cross the toolbar.
      await page.setViewportSize({ width, height: width > 700 ? 180 : 240 });
      const inlineHeading = page.locator("#keenetic-policy-title");
      const add = page.getByRole("button", { name: "Add profile", exact: true });
      async function placeHeading(offset: number) {
        await page.evaluate((offset) => {
          const title = document.querySelector("#keenetic-policy-title")!.getBoundingClientRect();
          const toolbar = document.querySelector(".settings-toolbar")!.getBoundingClientRect();
          window.scrollBy(0, title.bottom - toolbar.height - offset);
        }, offset);
      }
      await placeHeading(36);
      await expect(heading).toHaveAccessibleName("Interface priority profiles");
      await expect(inlineHeading).toBeInViewport();
      await expect(add).toBeVisible();
      await expect(page.locator(".context-add")).toHaveCSS("opacity", "1");

      // Crossing the boundary starts the title fade and the Add transition, even if scrolling stops.
      const titleNode = await inlineHeading.elementHandle();
      await placeHeading(-4);
      await expect(heading).toHaveAccessibleName("Keenetic policy");
      const stoppedAt = await page.evaluate(() => window.scrollY);
      await expect(page.locator(".context-add")).toHaveCSS("opacity", "0");
      await expect(page.locator(".title-layer.current")).toHaveCSS("opacity", "1");
      await expect(page.locator(".title-layer:not(.current)")).toHaveCSS("opacity", "0");
      await expect(page.locator(".title-layer.current")).toHaveText("Keenetic policy");
      expect(await page.evaluate(() => window.scrollY)).toBe(stoppedAt);
      await expect(add).toHaveCount(0);
      // The section heading stays in normal document flow; no floating copies or position updates.
      await expect(inlineHeading).toHaveCSS("position", "static");
      for (const offset of [36, -8, 44, -16, 36]) {
        await placeHeading(offset);
        await expect(heading).toHaveAccessibleName(
          offset > 0 ? "Interface priority profiles" : "Keenetic policy",
        );
        expect(
          await titleNode!.evaluate(
            (node) => node === document.querySelector("#keenetic-policy-title"),
          ),
        ).toBe(true);
      }
      await expect(page.locator(".title-layer.current")).toHaveCSS("opacity", "1");
      await expect(page.locator(".title-layer:not(.current)")).toHaveCSS("opacity", "0");
      await expect(page.locator(".title-layer.current")).toHaveText("Interface priority profiles");
      await expect(heading).toHaveAccessibleName("Interface priority profiles");
      await expect(add).toBeVisible();
      await expect(page.locator(".context-add")).toHaveCSS("opacity", "1");
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      await expect(heading).toHaveAccessibleName("Keenetic policy");
      await expect(add).toHaveCount(0);
      await expect(page.locator(".settings-page .page-controls")).toHaveCount(1);
      await expect(save(page)).toHaveCount(1);
      await expect(save(page)).toBeEnabled();
      expect(state.writes).toBe(0);
      expect(state.profileWrites).toBe(0);
      // The common button survives the transition rather than being replaced
      // by a second section-specific Save, even during rapid scroll changes.
      expect(
        await button!.evaluate((node) => node === document.querySelector("#save-settings")),
      ).toBe(true);
      await page.setViewportSize({ width, height: 900 });
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      await expect(heading).toHaveAccessibleName("Interface priority profiles");
      await expect(name).toHaveValue("Unsaved profile name");
      await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
      // Wait for both API writes: Save is disabled during the first request
      // as well as after completion, so its disabled state is not an ack.
      await save(page).click();
      await expect.poll(() => state.profileWrites).toBe(1);
      await expect.poll(() => state.writes).toBe(1);
      await expect(save(page)).toBeDisabled();
      expect(state.profiles[0].name).toBe("Unsaved profile name");
      await page.setViewportSize({ width, height: 900 });
      await page.evaluate(() => window.scrollTo(0, 0));
      await expect(heading).toHaveAccessibleName("Interface priority profiles");
      await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
      await expect(name).toHaveValue("Unsaved profile name");
      await expect(save(page)).toBeDisabled();
      expect(state.settings.mode).toBe("exclude");
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
        width,
      );
    });
  }
  test("short pages select the editor by keyboard focus, also with reduced motion", async ({
    page,
  }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    const state = await backend(page);
    await openSettings(page);
    const heading = page.locator(".settings-toolbar").getByRole("heading", { level: 2 });
    const toggle = panel(page).getByRole("switch", {
      name: "Use policy based filter",
      exact: true,
    });
    await toggle.focus();
    await expect(heading).toHaveAccessibleName("Interface priority profiles");
    await expect(panel(page).getByRole("heading", { name: "Keenetic policy" })).toBeInViewport();
    await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
    await expect(save(page)).toBeDisabled();
    await toggle.press("Space");
    await expect(toggle).toBeChecked();
    expect(state.writes).toBe(0);
    await toggle.press("Space");
    await expect(save(page)).toBeDisabled();
    await expect(page.locator(".settings-page .page-controls")).toHaveCount(1);
    await expect(save(page)).toHaveCount(1);
    await page.getByRole("button", { name: "Add profile", exact: true }).click();
    await expect(page.getByPlaceholder("Profile name", { exact: true })).toHaveCount(1);
    expect(state.profileWrites).toBe(0);
  });
});


test.describe("Save applies to the entire Settings page", () => {
  test.skip(!keenetic, "Keenetic-only editor");
  test("a profile edit remains saveable while viewing policy", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 540 });
    const state = await backend(page, true);
    state.profiles = [{
      id: "one", name: "Original", interfaces: ["tun0"], on_unavailable: "blackhole",
    }];
    await openSettings(page);
    await page.getByPlaceholder("Profile name", { exact: true }).fill("New name");
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    await expect(save(page)).toBeEnabled();
    await save(page).click();
    await expect.poll(() => state.profileWrites).toBe(1);
    expect(state.writes).toBe(0);
    expect(state.profiles[0].name).toBe("New name");
    await expect(save(page)).toBeDisabled();
  });

  test("a policy edit remains saveable while viewing profiles", async ({ page }) => {
    const state = await backend(page, true);
    await openSettings(page);
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    await page.evaluate(() => window.scrollTo(0, 0));
    await expect(save(page)).toBeEnabled();
    await save(page).click();
    await expect.poll(() => state.writes).toBe(1);
    expect(state.profileWrites).toBe(0);
    expect(state.settings.mode).toBe("exclude");
    await expect(save(page)).toBeDisabled();
  });

  test("invalid changes block all writes until corrected", async ({ page }) => {
    const state = await backend(page, true);
    state.profiles = [{
      id: "one", name: "Original", interfaces: ["tun0"], on_unavailable: "blackhole",
    }];
    await openSettings(page);
    const name = page.getByPlaceholder("Profile name", { exact: true });
    await name.fill("");
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    await expect(save(page)).toBeDisabled();
    expect(state.profileWrites).toBe(0);
    expect(state.writes).toBe(0);
    await name.fill("Repaired");
    await expect(save(page)).toBeEnabled();
    await save(page).click();
    await expect.poll(() => state.profileWrites).toBe(1);
    await expect.poll(() => state.writes).toBe(1);
    expect(state.profiles[0].name).toBe("Repaired");
    await expect(save(page)).toBeDisabled();
  });

  test("partial disk failure retries only the remaining dirty section", async ({ page }) => {
    const state = await backend(page, true);
    state.profiles = [{
      id: "one", name: "Original", interfaces: ["tun0"], on_unavailable: "blackhole",
    }];
    await openSettings(page);
    await page.getByPlaceholder("Profile name", { exact: true }).fill("Saved once");
    await panel(page).getByRole("switch", { name: "Processing mode", exact: true }).click();
    state.diskFailure = true;
    await save(page).click();
    await expect.poll(() => state.profileWrites).toBe(1);
    await expect.poll(() => state.writes).toBe(1);
    await expect(save(page)).toBeEnabled();
    state.diskFailure = false;
    await save(page).click();
    await expect.poll(() => state.writes).toBe(2);
    expect(state.profileWrites).toBe(1);
    await expect(save(page)).toBeDisabled();
  });
});
