import { expect, test, type Page } from "@playwright/test";

import type { UpdateStatus } from "../../src/modules/updates/releases";

const job = (stage: UpdateStatus["stage"]): UpdateStatus => ({
  stage,
  job_id: "a".repeat(32),
  tag: "2.0.0",
  target_version: "2.0.0",
  target_revision: 1,
});

async function setup(page: Page) {
  const state = {
    status: { stage: "idle" } as UpdateStatus,
    installed: "1.0.0",
    release: "2.0.0",
    previewRelease: "4.0.0",
    offline: false,
    failedInfoReads: 0,
    infoReads: 0,
    releaseReads: 0,
    installs: 0,
  };
  await page.route("**/auth", (route) => route.fulfill({ json: { enabled: false } }));
  await page.route("**/groups?with_rules=true", (route) => route.fulfill({ json: { groups: [] } }));
  await page.route("**/interfaces", (route) => route.fulfill({ json: { interfaces: [] } }));
  await page.route("**/system/update", (route) => {
    state.infoReads++;
    if (state.failedInfoReads > 0) {
      state.failedInfoReads--;
      return route.abort("connectionfailed");
    }
    return route.fulfill({
      json: {
        installed_version: state.installed,
        installed_revision: 1,
        asset_suffix: "test.ipk",
        can_install: true,
        reason: "",
      },
    });
  });
  await page.route("**/system/update/status", (route) => route.fulfill({ json: state.status }));
  await page.route("**/system/update/install", (route) => {
    state.installs++;
    state.status = job("downloading");
    return route.fulfill({ status: 202, json: { job_id: "a".repeat(32), stage: "queued" } });
  });
  await page.route("https://api.github.com/**", (route) => {
    state.releaseReads++;
    expect(route.request().url()).toContain("releases?per_page=100");
    if (state.offline) return route.fulfill({ status: 503, json: {} });
    const name = `mt-c_${state.release}-1_test.ipk`;
    const previewName = `mt-c_${state.previewRelease}-1_test.ipk`;
    const downloadBase = "https://github.com/dan0102dan/mt-c/releases/download";
    return route.fulfill({
      json: [
        {
          id: 100,
          tag_name: state.release,
          draft: false,
          prerelease: false,
          published_at: "2026-10-09T00:00:00Z",
          assets: [
            {
              name,
              browser_download_url: `${downloadBase}/${state.release}/${name}`,
              state: "uploaded",
              size: 123,
              digest: `sha256:${"a".repeat(64)}`,
            },
          ],
        },
        {
          id: 101,
          tag_name: state.previewRelease,
          draft: false,
          prerelease: true,
          published_at: "2026-10-10T00:00:00Z",
          assets: [
            {
              name: previewName,
              browser_download_url: `${downloadBase}/${state.previewRelease}/${previewName}`,
              state: "uploaded",
              size: 123,
              digest: `sha256:${"a".repeat(64)}`,
            },
          ],
        },
      ],
    });
  });
  return state;
}

async function openPopup(page: Page) {
  await page.getByRole("button", { name: /Software update:/ }).click();
  const popup = page.getByRole("dialog", { name: "Software update", exact: true });
  await expect(popup).toBeVisible();
  return popup;
}

test("retry a failed release check without reloading or installing anything", async ({ page }) => {
  const state = await setup(page);
  state.offline = true;
  await page.goto("/");
  const popup = await openPopup(page);
  const retry = popup.getByRole("button", { name: "Check for updates", exact: true });
  await expect(retry).toBeEnabled();
  const attempts = state.releaseReads;
  state.offline = false;
  await retry.click();
  await expect(popup.getByRole("button", { name: "Update", exact: true })).toBeEnabled();
  expect(state.releaseReads).toBe(attempts + 1);
  expect(state.infoReads).toBe(1);
  expect(state.installs).toBe(0);
  await page.keyboard.press("Escape");
  await openPopup(page);
  await expect(popup.getByRole("button", { name: "Update", exact: true })).toBeEnabled();
  expect(state.releaseReads).toBe(attempts + 1);
});

test("refresh an expired list while fresh opens and channel switches stay local", async ({
  page,
}) => {
  const state = await setup(page);
  await page.goto("/");
  const popup = await openPopup(page);
  await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
  await expect(popup.getByRole("switch", { name: "dev" })).toBeEnabled();
  expect(state.releaseReads).toBe(1);
  await page.keyboard.press("Escape");
  state.release = "3.0.0";
  await page.evaluate(() => {
    const later = Date.now() + 60 * 60 * 1000 + 1;
    Date.now = () => later;
  });
  await openPopup(page);
  await expect(popup.getByRole("link", { name: "3.0.0 · Release notes" })).toBeVisible();
  await expect(popup.getByRole("switch", { name: "dev" })).toBeEnabled();
  for (let i = 0; i < 4; i++) await popup.getByRole("switch", { name: "dev" }).click();
  expect(state.releaseReads).toBe(2);
  expect(state.infoReads).toBe(1);
});

test("a failed post-install version read retries only GET and keeps installation disabled", async ({
  page,
}) => {
  const state = await setup(page);
  await page.goto("/");
  const popup = await openPopup(page);
  await popup.getByRole("button", { name: "Update", exact: true }).click();
  await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
  state.installed = "2.0.0";
  state.failedInfoReads = 1;
  state.status = job("succeeded");
  await expect(
    popup.getByText("Installed version could not be confirmed", { exact: true }),
  ).toBeVisible();
  await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
  await expect(popup.getByRole("button", { name: "Up to date", exact: true })).toBeDisabled({
    timeout: 10000,
  });
  await expect(
    popup.getByText("Installed version could not be confirmed", { exact: true }),
  ).toHaveCount(0);
  expect(state.infoReads).toBeGreaterThanOrEqual(3);
  expect(state.installs).toBe(1);
});

test("reopening during a job which then fails restores the manual retry candidate", async ({
  page,
}) => {
  const state = await setup(page);
  state.status = job("downloading");
  await page.goto("/");
  const popup = await openPopup(page);
  await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
  await expect.poll(() => state.infoReads).toBeGreaterThanOrEqual(1);
  state.status = { ...job("failed"), error: "Package download failed" };
  await expect(popup.getByRole("button", { name: "Update", exact: true })).toBeEnabled({
    timeout: 10000,
  });
  expect(state.releaseReads).toBe(1);
  await popup.getByRole("button", { name: "Update", exact: true }).click();
  await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
  expect(state.installs).toBe(1);
});

test("dev channel switches locally and survives a reload", async ({ page }) => {
  const state = await setup(page);
  await page.goto("/");
  let popup = await openPopup(page);
  const toggle = popup.getByRole("switch", { name: "dev" });
  await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
  await toggle.click();
  await expect(toggle).toBeChecked();
  await expect(popup.getByRole("link", { name: "4.0.0 · Release notes" })).toBeVisible();
  await toggle.click();
  await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
  await toggle.click();
  expect(state.releaseReads).toBe(1);

  await page.reload();
  popup = await openPopup(page);
  await expect(popup.getByRole("switch", { name: "dev" })).toBeChecked();
  await expect(popup.getByRole("link", { name: "4.0.0 · Release notes" })).toBeVisible();
  expect(state.releaseReads).toBe(1);
  expect(state.infoReads).toBe(2);
});

test("update popup remains inside the mobile viewport", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await setup(page);
  await page.goto("/");
  const popup = await openPopup(page);
  const bounds = await popup.boundingBox();
  expect(bounds).not.toBeNull();
  expect(bounds!.x).toBeGreaterThanOrEqual(0);
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(390);
});
