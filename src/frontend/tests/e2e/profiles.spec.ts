import { expect, test, type Locator, type Page } from "@playwright/test";

import type { Group, Profile, Subscription } from "../../src/types";

async function backend(page: Page) {
  const state = {
    profiles: [] as Profile[],
    interfaces: ["blackhole", "tun0", "tun1", "tun2"].map((id) => ({ id, name: "" })),
    groups: [
      {
        id: "aabbccdd",
        name: "GitHub",
        color: "#ffffff",
        interface: "tun0",
        enable: true,
        rules: [
          { id: "00000001", name: "GitHub", rule: "github.com", type: "namespace", enable: true },
        ],
      },
    ] as Group[],
    subscriptions: [
      {
        id: "11223344",
        name: "Cloud networks",
        interface: "tun0",
        enable: true,
        url: "https://example.test/list",
        interval: 86400,
        lastUpdate: 1,
        rules: [{ id: "00000002", rule: "example.test", type: "namespace", enable: true }],
      },
    ] as Subscription[],
    failure: "" as "" | "reject" | "disk",
    writes: 0,
  };
  const canonical = <T extends { interface: string; profile?: string }>(value: T): T => {
    const profile = state.profiles.find((p) => p.id === value.profile);
    return profile ? { ...value, interface: profile.interfaces[0] } : value;
  };
  const responseProfiles = () =>
    state.profiles.map((p) => ({
      ...p,
      usage: {
        groups: state.groups.filter((g) => g.profile === p.id).length,
        subscriptions: state.subscriptions.filter((s) => s.profile === p.id).length,
      },
    }));
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname.replace("/api/v1", "");
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
        json: { interfaces: state.interfaces },
      });
      return;
    }
    if (path === "/profiles") {
      if (request.method() === "PUT") {
        state.writes++;
        if (state.failure === "reject") {
          await route.fulfill({ status: 409, json: { error: "Profile is in use" } });
          return;
        }
        state.profiles = request.postDataJSON().profiles;
        state.groups = state.groups.map(canonical);
        state.subscriptions = state.subscriptions.map(canonical);
        if (state.failure === "disk") {
          await route.fulfill({
            status: 500,
            json: {
              code: "PERSISTENCE_FAILED",
              applied: true,
              error: "Disk is full",
              profiles: responseProfiles(),
            },
          });
          return;
        }
      }
      await route.fulfill({ json: { profiles: responseProfiles() } });
      return;
    }
    if (path === "/groups") {
      if (request.method() === "PUT") {
        state.groups = request.postDataJSON().groups.map((g: Group) =>
          canonical({
            ...g,
            rules: g.rules ?? state.groups.find((x) => x.id === g.id)?.rules ?? [],
          }),
        );
      }
      await route.fulfill({ json: { groups: state.groups } });
      return;
    }
    if (path === "/subscriptions") {
      if (request.method() === "PUT") {
        state.subscriptions = request.postDataJSON().subscriptions.map((s: Subscription) =>
          canonical({
            ...s,
            rules: s.rules ?? state.subscriptions.find((x) => x.id === s.id)?.rules ?? [],
          }),
        );
      }
      await route.fulfill({ json: { subscriptions: state.subscriptions } });
      return;
    }
    await route.fulfill({ json: { version: "test" } });
  });
  return state;
}

async function openSettings(page: Page) {
  if ((page.viewportSize()?.width ?? 1280) <= 700)
    await page.getByRole("button", { name: "Open menu", exact: true }).click();
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeEnabled();
}
async function addInterface(card: Locator, name: string) {
  await card.locator('[data-select-trigger][aria-label="Add interface"]').click();
  await card.getByRole("option", { name: new RegExp(`^${name}(\\s|$)`) }).click();
  await expect(card.locator(`[data-interface="${name}"]`)).toBeVisible();
}

async function dragInterface(page: Page, source: Locator, target: Locator, after = false) {
  const from = await source.locator(".grip").boundingBox();
  const to = await target.boundingBox();
  if (!from || !to) throw new Error("Interface row not found");
  await page.mouse.move(from.x + from.width / 2, from.y + from.height / 2);
  await page.mouse.down();
  await page.mouse.move(to.x + to.width / 2, to.y + to.height * (after ? 0.75 : 0.25), {
    steps: 10,
  });
  await page.mouse.up();
}

async function create(page: Page, name = "VPN with fallback") {
  await openSettings(page);
  await page.getByRole("button", { name: "Add profile", exact: true }).click();
  const card = page.locator(".profile").last();
  await card.getByLabel("Profile name", { exact: true }).fill(name);
  for (const name of ["tun0", "tun1", "tun2"]) await addInterface(card, name);
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
}

test("create profiles in Settings, select one in groups and subscriptions, save and reload", async ({
  page,
}) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page);
  expect(state.profiles).toHaveLength(1);
  expect(state.profiles[0].interfaces).toEqual(["tun0", "tun1", "tun2"]);
  await page.getByRole("tab", { name: "Groups", exact: true }).click();
  await page.locator(".group-header").locator('[data-select-trigger][aria-label="Route"]').click();
  await page.getByRole("option", { name: "VPN with fallback", exact: true }).click();
  await page.locator("#save-changes").click();
  await expect.poll(() => state.groups[0].profile).toBe(state.profiles[0].id);
  expect(state.groups[0].interface).toBe("tun0");
  await page.getByRole("tab", { name: "Subscriptions", exact: true }).click();
  await page
    .locator(".subscription-actions")
    .locator('[data-select-trigger][aria-label="Route"]')
    .click();
  await page.getByRole("option", { name: "VPN with fallback", exact: true }).click();
  await page.locator("#save-subscriptions").click();
  await expect.poll(() => state.subscriptions[0].profile).toBe(state.profiles[0].id);
  expect(state.subscriptions[0].interface).toBe("tun0");
  await page.reload();
  await expect(
    page.locator(".subscription-actions").locator('[data-select-trigger][aria-label="Route"]'),
  ).toHaveText("VPN with fallback");
  await openSettings(page);
  await expect(
    page.getByRole("button", { name: "Delete profile: VPN with fallback", exact: true }),
  ).toBeDisabled();
  await expect(page.locator(".usage")).toHaveText("1 groups, 1 subscriptions");
});

test("reorder profile changes compatibility primary; direct selection clears profile", async ({
  page,
}) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page);
  state.groups[0].profile = state.profiles[0].id;
  state.subscriptions[0].profile = state.profiles[0].id;
  await dragInterface(
    page,
    page.locator('[data-interface="tun1"]'),
    page.locator('[data-interface="tun0"]'),
  );
  await expect(page.locator(".interface-id")).toHaveText(["tun1", "tun0", "tun2"]);
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.groups[0].interface).toBe("tun1");
  expect(state.subscriptions[0].interface).toBe("tun1");
  await page.getByRole("tab", { name: "Groups", exact: true }).click();
  await page.locator(".group-header").locator('[data-select-trigger][aria-label="Route"]').click();
  await page.getByRole("option", { name: "blackhole", exact: true }).click();
  await page.locator("#save-changes").click();
  await expect.poll(() => state.groups[0].interface).toBe("blackhole");
  expect(state.groups[0].profile).toBeUndefined();
});

test("draft survives tab navigation without a footer; invalid empty chain cannot save", async ({
  page,
}) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page);
  await page.getByLabel("Profile name", { exact: true }).fill("Unsaved rename");
  await page.getByRole("tab", { name: "Groups", exact: true }).click();
  await openSettings(page);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Unsaved rename");
  await expect(page.getByRole("button", { name: "Cancel", exact: true })).toHaveCount(0);
  await expect(page.locator(".profiles-settings footer")).toHaveCount(0);
  await expect(page.getByText("Unsaved changes", { exact: true })).toHaveCount(0);
  expect(state.profiles[0].name).toBe("VPN with fallback");
  for (const name of ["tun2", "tun1", "tun0"])
    await page.getByRole("button", { name: `Remove interface: ${name}`, exact: true }).click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles[0].interfaces).toHaveLength(3);
  await expect(page.getByText("Add at least one interface", { exact: true })).toHaveCount(0);
  await expect(page.locator("#save-profiles")).toHaveAccessibleName("Add at least one interface");
  await addInterface(page.locator(".profile"), "tun1");
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles[0].name).toBe("Unsaved rename");
  expect(state.profiles[0].interfaces).toEqual(["tun1"]);
});

test("server rejection retains draft; applied disk failure remains retryable", async ({ page }) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page);
  state.failure = "reject";
  await page.getByLabel("Profile name", { exact: true }).fill("Updated VPN");
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeEnabled();
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Updated VPN");
  expect(state.profiles[0].name).toBe("VPN with fallback");
  await expect(page.locator(".toast").filter({ hasText: "Profile is in use" })).toContainText(
    "Profile is in use",
  );
  state.failure = "disk";
  await page.locator("#save-profiles").click();
  await expect(
    page
      .locator(".toast")
      .filter({ hasText: "Changes are applied but not saved to disk. Retry saving." }),
  ).toBeVisible();
  await expect(page.locator("#save-profiles")).toBeEnabled();
  state.failure = "";
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles[0].name).toBe("Updated VPN");
});

test("profile section uses a placeholder, plain interface rows and a fixed terminal row", async ({
  page,
}) => {
  const state = await backend(page);
  state.interfaces[1].name = "Keenetic VPN description";
  await page.goto("/");
  await create(page);
  await expect(page.locator(".settings-page [role=tablist]")).toHaveCount(0);
  await expect(page.locator(".settings-page [role=tab]")).toHaveCount(0);
  await expect(
    page.getByRole("heading", { name: "Interface priority profiles", level: 2 }),
  ).toBeVisible();
  const card = page.locator(".profile");
  await expect(card.getByPlaceholder("Profile name", { exact: true })).toHaveValue(
    "VPN with fallback",
  );
  await expect(card.locator("label")).toHaveCount(0);
  await expect(card.locator(".usage")).toHaveText("0 groups, 0 subscriptions");
  await expect(card.locator(".usage svg")).toBeVisible();
  await expect(card.locator(".usage")).toHaveAttribute("title", "Used by");
  await expect(card.locator(".profile-header .add-interface")).toBeVisible();
  await expect(card.locator("[data-collapsible-content] .add-interface")).toHaveCount(0);
  await expect(card.locator(".interface [data-select-trigger]")).toHaveCount(0);
  await expect(card.locator('[data-interface="tun0"] .interface-description')).toHaveText(
    "Keenetic VPN description",
  );
  await expect(card.locator('[data-interface="tun1"] .interface-description')).toHaveCount(0);
  const terminal = card.locator('.interface-table [role="row"]').last();
  await expect(terminal).toHaveClass(/terminal/);
  await expect(terminal).toHaveAttribute("aria-disabled", "true");
  await expect(terminal.locator("button, [data-droppable], [draggable=true]")).toHaveCount(0);
  await expect(terminal).not.toHaveAttribute("draggable", "true");
  await expect(card.locator(".add-interface [data-select-trigger]")).toBeDisabled();
  await card.getByRole("button", { name: "Remove interface: tun1", exact: true }).click();
  await card.locator(".add-interface [data-select-trigger]").click();
  await expect(card.getByRole("option")).toHaveCount(1);
  await expect(card.getByRole("option", { name: "tun1", exact: true })).toBeVisible();
  await card.getByRole("option", { name: "tun1", exact: true }).click();
  await expect(card.locator(".interface-id")).toHaveText(["tun0", "tun2", "tun1"]);
  // Re-adding the same interface must remain possible after another removal.
  await card.getByRole("button", { name: "Remove interface: tun1", exact: true }).click();
  await addInterface(card, "tun1");
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles[0].interfaces).toEqual(["tun0", "tun2", "tun1"]);
  await page.reload();
  await expect(card.locator(".interface-id")).toHaveText(["tun0", "tun2", "tun1"]);
});

test("dragging stays inside its profile and cannot move blackhole; keyboard reorder works too", async ({
  page,
}) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page, "First profile");
  await create(page, "Second profile");
  const first = page.locator(".profile").nth(0);
  const second = page.locator(".profile").nth(1);
  await dragInterface(
    page,
    first.locator('[data-interface="tun0"]'),
    second.locator('[data-interface="tun2"]'),
    true,
  );
  await expect(first.locator(".interface-id")).toHaveText(["tun0", "tun1", "tun2"]);
  await expect(second.locator(".interface-id")).toHaveText(["tun0", "tun1", "tun2"]);
  await dragInterface(
    page,
    first.locator('[data-interface="tun0"]'),
    first.locator(".terminal"),
    true,
  );
  await expect(first.locator(".interface-id")).toHaveText(["tun0", "tun1", "tun2"]);
  await dragInterface(
    page,
    first.locator('[data-interface="tun0"]'),
    first.locator('[data-interface="tun2"]'),
    true,
  );
  await expect(first.locator(".interface-id")).toHaveText(["tun1", "tun2", "tun0"]);
  await first
    .getByRole("button", { name: "Drag interface: tun2", exact: true })
    .press("Alt+ArrowUp");
  await expect(first.locator(".interface-id")).toHaveText(["tun2", "tun1", "tun0"]);
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles.map((p) => p.interfaces)).toEqual([
    ["tun2", "tun1", "tun0"],
    ["tun0", "tun1", "tun2"],
  ]);
});

test("mobile Settings keeps add controls visible and allows an empty draft to be filled", async ({
  page,
}) => {
  const state = await backend(page);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/");
  await openSettings(page);
  await page.getByRole("button", { name: "Add profile", exact: true }).click();
  const card = page.locator(".profile");
  await card.getByPlaceholder("Profile name", { exact: true }).fill("Mobile profile");
  await expect(page.locator("#save-profiles")).toBeDisabled();
  await expect(card.locator(".add-interface [data-select-trigger]")).toBeVisible();
  await addInterface(card, "tun2");
  await expect(card.locator(".order").first()).toHaveText("1");
  await page.locator("#save-profiles").click();
  await expect(page.locator("#save-profiles")).toBeDisabled();
  expect(state.profiles[0].interfaces).toEqual(["tun2"]);
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
});

test("page controls and first cards stay aligned when switching tabs", async ({ page }) => {
  const state = await backend(page);
  state.profiles = [
    { id: "layout", name: "Fallback", interfaces: ["tun0"], on_unavailable: "blackhole" },
  ];
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("/");

  const positions: { toolbarTop: number; toolbarHeight: number; cardTop: number }[] = [];
  for (const [tab, cardSelector] of [
    ["Groups", ".group-wrapper:not(.is-hidden) .group"],
    ["Subscriptions", ".subscription-wrapper:not(.is-hidden) .subscription-panel"],
    ["Settings", ".profiles-settings .profile"],
    ["Groups", ".group-wrapper:not(.is-hidden) .group"],
  ] as const) {
    await page.getByRole("tab", { name: tab, exact: true }).click();
    const active = page.locator('.app-tab-content[data-state="active"]');
    const toolbar = active.locator(".page-controls");
    const card = active.locator(cardSelector).first();
    await expect(toolbar).toBeVisible();
    await expect(card).toBeVisible();

    const toolbarBox = await toolbar.boundingBox();
    const cardBox = await card.boundingBox();
    if (!toolbarBox || !cardBox) throw new Error(`Missing layout for ${tab}`);
    positions.push({
      toolbarTop: toolbarBox.y,
      toolbarHeight: toolbarBox.height,
      cardTop: cardBox.y,
    });
  }

  const initial = positions[0];
  for (const current of positions.slice(1)) {
    expect(Math.abs(current.toolbarTop - initial.toolbarTop)).toBeLessThan(1);
    expect(Math.abs(current.toolbarHeight - initial.toolbarHeight)).toBeLessThan(1);
    expect(Math.abs(current.cardTop - initial.cardTop)).toBeLessThan(2);
  }
});

for (const width of [1280, 390]) {
  test(`header add menu works while collapsed and over the next profile (${width}px)`, async ({
    page,
  }) => {
    const state = await backend(page);
    state.profiles = [
      { id: "first", name: "First profile", interfaces: ["tun0"], on_unavailable: "blackhole" },
      { id: "second", name: "Second profile", interfaces: ["tun1"], on_unavailable: "blackhole" },
    ];
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/");
    await openSettings(page);
    const first = page.locator(".profile").first();
    const second = page.locator(".profile").last();
    await first.getByRole("button", { name: "Collapse", exact: true }).click();
    await expect(first.locator(".interface-table")).not.toBeVisible();
    const picker = first.locator(".profile-header .add-interface [data-select-trigger]");
    await expect(picker).toBeInViewport();
    await addInterface(first, "tun2");
    await expect(first.locator(".interface-id")).toHaveText(["tun0", "tun2"]);
    await expect(second.locator(".interface-id")).toHaveText(["tun1"]);
    await page.locator("#save-profiles").click();
    await expect(page.locator("#save-profiles")).toBeDisabled();
    expect(state.profiles[0].interfaces).toEqual(["tun0", "tun2"]);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      width,
    );
  });
}
