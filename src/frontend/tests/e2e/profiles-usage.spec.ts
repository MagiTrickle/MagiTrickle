import { expect, test, type Page } from "@playwright/test";

import type { Group, Profile, Subscription } from "../../src/types";

// Server usage is calculated from applied consumers, never from UI drafts.
async function backend(page: Page, subscriptionUsesProfile = false) {
  const state = {
    profiles: [
      { id: "shared", name: "Shared VPN", interfaces: ["tun0"], on_unavailable: "blackhole" },
    ] as Profile[],
    groups: [
      {
        id: "aabbccdd",
        name: "Sites",
        color: "#ffffff",
        interface: "tun0",
        profile: "shared",
        enable: true,
        rules: [
          { id: "00000001", name: "Site", rule: "example.test", type: "namespace", enable: true },
        ],
      },
    ] as Group[],
    subscriptions: [
      {
        id: "11223344",
        name: "Clouds",
        interface: "tun0",
        profile: subscriptionUsesProfile ? "shared" : undefined,
        enable: true,
        url: "https://example.test/list",
        interval: 86400,
        lastUpdate: 1,
        rules: [{ id: "00000002", rule: "example.test", type: "namespace", enable: true }],
      },
    ] as Subscription[],
    failure: "" as "" | "reject" | "disk",
    failProfileRead: false,
    profileReads: 0,
    holdNextRead: false,
    heldReads: 0,
    releaseRead: undefined as (() => void) | undefined,
  };
  const responseProfiles = () =>
    state.profiles.map((profile) => ({
      ...profile,
      usage: {
        groups: state.groups.filter((group) => group.profile === profile.id).length,
        subscriptions: state.subscriptions.filter((sub) => sub.profile === profile.id).length,
      },
    }));
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    const write = request.method() === "PUT";
    if (request.method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
      return;
    }
    if (path === "/auth") {
      await route.fulfill({ json: { enabled: false } });
      return;
    }
    if (path === "/system/interfaces") {
      await route.fulfill({
        json: { interfaces: ["blackhole", "tun0"].map((id) => ({ id, name: "" })) },
      });
      return;
    }
    if (path === "/profiles") {
      if (write) {
        const next = request.postDataJSON().profiles as Profile[];
        const removedUsed = responseProfiles().some(
          (profile) =>
            !next.some((candidate) => candidate.id === profile.id) &&
            (profile.usage.groups > 0 || profile.usage.subscriptions > 0),
        );
        if (removedUsed) {
          await route.fulfill({ status: 409, json: { error: "Profile is in use" } });
          return;
        }
        state.profiles = next;
      } else {
        state.profileReads++;
        if (state.failProfileRead) {
          await route.fulfill({ status: 500, json: { error: "Usage unavailable" } });
          return;
        }
      }
      const response = responseProfiles();
      if (!write && state.holdNextRead) {
        state.holdNextRead = false;
        state.heldReads++;
        await new Promise<void>((resolve) => {
          state.releaseRead = resolve;
        });
      }
      await route.fulfill({ json: { profiles: response } });
      return;
    }
    if (path === "/groups" || path === "/subscriptions") {
      if (write && state.failure === "reject") {
        await route.fulfill({ status: 409, json: { error: "Configuration conflict" } });
        return;
      }
      if (write && path === "/groups") {
        const values = request.postDataJSON().groups as Group[];
        state.groups = values.map((group) => {
          const previous = state.groups.find((before) => before.id === group.id);
          return { ...group, rules: group.rules ?? previous?.rules ?? [] };
        });
      }
      if (write && path === "/subscriptions") {
        const values = request.postDataJSON().subscriptions as Subscription[];
        state.subscriptions = values.map((sub) => {
          const previous = state.subscriptions.find((before) => before.id === sub.id);
          return { ...sub, rules: sub.rules ?? previous?.rules ?? [] };
        });
      }
      const payload =
        path === "/groups" ? { groups: state.groups } : { subscriptions: state.subscriptions };
      await route.fulfill({
        status: write && state.failure === "disk" ? 500 : 200,
        json:
          write && state.failure === "disk"
            ? { ...payload, code: "PERSISTENCE_FAILED", applied: true, error: "Disk full" }
            : payload,
      });
      return;
    }
    await route.fulfill({ json: { version: "test" } });
  });
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("/");
  return state;
}

const deleteProfile = (page: Page) => page.getByRole("button", { name: /^Delete profile:/ });

async function settings(page: Page) {
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeEnabled();
}

async function unlink(page: Page, kind: "group" | "subscription" = "group") {
  const tab = kind === "group" ? "Groups" : "Subscriptions";
  const header = kind === "group" ? ".group-header" : ".subscription-actions";
  const save = kind === "group" ? "#save-changes" : "#save-subscriptions";
  const path = kind === "group" ? "/groups" : "/subscriptions";
  await page.getByRole("tab", { name: tab, exact: true }).click();
  await page.locator(header).locator('[data-select-trigger][aria-label="Route"]').click();
  await page.getByRole("option", { name: /^tun0(\s|$)/ }).click();
  const saved = page.waitForResponse((response) => {
    const pathname = new URL(response.url()).pathname;
    return pathname.endsWith(path) && response.request().method() === "PUT";
  });
  await page.locator(save).click();
  await saved;
}

test("saved group unlink releases the profile without page reload", async ({ page }) => {
  const state = await backend(page);
  await settings(page);
  await expect(deleteProfile(page)).toBeDisabled();
  await unlink(page);
  expect(state.groups[0].profile).toBeUndefined();
  await settings(page);
  await expect(deleteProfile(page)).toBeEnabled();
  page.once("dialog", (dialog) => void dialog.accept());
  await deleteProfile(page).click();
  await expect(page.locator(".profile")).toHaveCount(0);
  await page.locator("#save-profiles").click();
  await expect.poll(() => state.profiles.length).toBe(0);
});

test("the last subscription reference still blocks deletion", async ({ page }) => {
  const state = await backend(page, true);
  await settings(page);
  await unlink(page);
  await settings(page);
  await expect(deleteProfile(page)).toBeDisabled();
  await unlink(page, "subscription");
  expect(state.subscriptions[0].profile).toBeUndefined();
  await settings(page);
  await expect(deleteProfile(page)).toBeEnabled();
});

test("usage refresh preserves an unsaved profile editor", async ({ page }) => {
  const state = await backend(page);
  await settings(page);
  await page.getByLabel("Profile name", { exact: true }).fill("Unsaved name");
  await unlink(page);
  await settings(page);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Unsaved name");
  await expect(page.locator("#save-profiles")).toBeEnabled();
  await expect(deleteProfile(page)).toBeEnabled();
  expect(state.profiles[0].name).toBe("Shared VPN");
});

for (const failure of ["reject", "disk"] as const) {
  test(`usage follows applied state after a consumer save ${failure}`, async ({ page }) => {
    const state = await backend(page);
    await settings(page);
    state.failure = failure;
    await unlink(page);
    await settings(page);
    if (failure === "reject") {
      expect(state.groups[0].profile).toBe("shared");
      await expect(deleteProfile(page)).toBeDisabled();
    } else {
      expect(state.groups[0].profile).toBeUndefined();
      await expect(deleteProfile(page)).toBeEnabled();
    }
    await page.getByRole("tab", { name: "Groups", exact: true }).click();
    await expect(page.locator("#save-changes")).toBeEnabled();
  });
}

test("an old in-flight response cannot satisfy a new usage refresh", async ({ page }) => {
  const state = await backend(page);
  await settings(page);
  await page.getByRole("tab", { name: "Groups", exact: true }).click();
  state.holdNextRead = true;
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await expect.poll(() => state.heldReads).toBe(1);
  try {
    await unlink(page);
    const readsBeforeReturn = state.profileReads;
    await page.getByRole("tab", { name: "Settings", exact: true }).click();
    state.releaseRead?.();
    await expect.poll(() => state.profileReads).toBeGreaterThan(readsBeforeReturn);
    await expect(deleteProfile(page)).toBeEnabled();
  } finally {
    state.releaseRead?.();
  }
});

test("failed refresh cannot enable deletion using a stale zero count", async ({ page }) => {
  const state = await backend(page);
  await unlink(page);
  await settings(page);
  await expect(deleteProfile(page)).toBeEnabled();
  await page.getByRole("tab", { name: "Groups", exact: true }).click();
  // Another client has attached a consumer since our last successful read.
  state.groups[0].profile = "shared";
  state.failProfileRead = true;
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  const section = page.locator(".profiles-settings");
  await expect(section.getByText("Failed to load profiles")).toBeVisible();
  await expect(deleteProfile(page)).toBeDisabled();
  state.failProfileRead = false;
  await section.getByRole("button", { name: "Retry" }).click();
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeEnabled();
  await expect(deleteProfile(page)).toBeDisabled();
});
