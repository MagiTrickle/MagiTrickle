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
  const card = page.locator(".profile").first();
  await card.getByLabel("Profile name", { exact: true }).fill(name);
  for (const name of ["tun0", "tun1", "tun2"]) await addInterface(card, name);
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
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
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
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
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.profiles[0].interfaces).toHaveLength(3);
  await expect(page.locator("#save-settings")).toHaveAccessibleDescription(
    "Profiles: Add at least one interface",
  );
  await expect(page.locator("#save-settings")).toHaveAccessibleName("Save Changes");
  await expect(page.locator("#save-settings")).toHaveClass(/inactive/);
  await addInterface(page.locator(".profile"), "tun1");
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.profiles[0].name).toBe("Unsaved rename");
  expect(state.profiles[0].interfaces).toEqual(["tun1"]);
});

test("server rejection retains draft; applied disk failure remains retryable", async ({ page }) => {
  const state = await backend(page);
  await page.goto("/");
  await create(page);
  state.failure = "reject";
  await page.getByLabel("Profile name", { exact: true }).fill("Updated VPN");
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Updated VPN");
  expect(state.profiles[0].name).toBe("VPN with fallback");
  await expect(page.locator(".toast").filter({ hasText: "Profile is in use" })).toContainText(
    "Profile is in use",
  );
  state.failure = "disk";
  await page.locator("#save-settings").click();
  await expect(
    page
      .locator(".toast")
      .filter({ hasText: "Changes are applied but not saved to disk. Retry saving." }),
  ).toBeVisible();
  await expect(page.locator("#save-settings")).toBeEnabled();
  state.failure = "";
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.profiles[0].name).toBe("Updated VPN");
});

test("profile section uses a placeholder, plain interface rows and a fixed terminal row", async ({
  page,
}) => {
  const state = await backend(page);
  state.interfaces[1].name = "Keenetic VPN description";
  await page.goto("/");
  await create(page);
  await expect(page.locator(".settings-page [role=tablist]")).toHaveCount(1);
  await expect(page.getByRole("tab", { name: "Profiles", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await expect(
    page.getByRole("heading", { name: "Profiles", level: 2, exact: true }),
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
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
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
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
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
  await expect(page.locator("#save-settings")).toBeDisabled();
  await expect(card.locator(".add-interface [data-select-trigger]")).toBeVisible();
  await addInterface(card, "tun2");
  await expect(card.locator(".order").first()).toHaveText("1");
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect(page.locator("#save-settings")).toBeInViewport();
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.profiles[0].interfaces).toEqual(["tun2"]);
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
});

for (const width of [390, 320]) {
  test(`mobile Settings action buttons are separated and fit at ${width}px`, async ({ page }) => {
    await backend(page);
    await page.setViewportSize({ width, height: 844 });
    await page.goto("/");
    await openSettings(page);

    const actions = [
      page.locator("#save-settings"),
      page.getByRole("button", { name: "Import Config", exact: true }),
      page.getByRole("button", { name: "Export Config", exact: true }),
      page.getByRole("button", { name: "Add profile", exact: true }),
    ];
    for (const action of actions) await expect(action).toBeVisible();
    const boxes = await Promise.all(actions.map((action) => action.boundingBox()));
    if (boxes.some((box) => !box)) throw new Error("Missing action geometry");
    for (let i = 0; i < boxes.length - 1; i++) {
      const previous = boxes[i]!;
      const next = boxes[i + 1]!;
      expect(next.x - (previous.x + previous.width)).toBeGreaterThanOrEqual(6);
      expect(next.x + next.width).toBeLessThanOrEqual(width);
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      width,
    );
  });
}

test("page controls and first cards stay aligned when switching tabs", async ({ page }) => {
  const state = await backend(page);
  state.profiles = [
    { id: "layout", name: "Fallback", interfaces: ["tun0"], on_unavailable: "blackhole" },
  ];
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("/");

  const positions: {
    toolbarTop: number;
    toolbarHeight: number;
    cardTop: number;
    settings: boolean;
  }[] = [];
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
      settings: tab === "Settings",
    });
  }

  const initial = positions[0];
  for (const current of positions.slice(1)) {
    expect(Math.abs(current.toolbarTop - initial.toolbarTop)).toBeLessThan(1);
    expect(Math.abs(current.toolbarHeight - initial.toolbarHeight)).toBeLessThan(1);
    if (current.settings) {
      expect(current.cardTop).toBeGreaterThan(current.toolbarTop + current.toolbarHeight);
    } else {
      expect(Math.abs(current.cardTop - initial.cardTop)).toBeLessThan(2);
    }
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
    await page.locator("#save-settings").click();
    await expect(page.locator("#save-settings")).toBeDisabled();
    await expect.poll(() => state.profiles[0].interfaces).toEqual(["tun0", "tun2"]);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      width,
    );
  });
}

async function geometry(locator: Locator) {
  const box = await locator.boundingBox();
  if (!box) throw new Error("Missing layout element");
  return box;
}

async function navigate(page: Page, name: string) {
  const menu = page.locator(".mobile-dropdown-btn");
  if ((await menu.isVisible()) && (await menu.getAttribute("aria-expanded")) !== "true")
    await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

for (const width of [390, 1280]) {
  test(`missing subscription interface uses the group highlight at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const state = await backend(page);
    state.groups[0].interface = state.subscriptions[0].interface = "offline0";
    await page.goto("/");
    const route = page.getByRole("button", { name: "Route", exact: true });
    await expect(route).toContainText("offline0");
    const missingColor = await route.evaluate((el) => getComputedStyle(el).color);

    await navigate(page, "Subscriptions");
    await expect(route).toContainText("offline0");
    await expect(route).toHaveCSS("color", missingColor);
    await route.click();
    await page.getByRole("option", { name: "tun1", exact: true }).click();
    await expect(route).toContainText("tun1");
    await expect(route).not.toHaveCSS("color", missingColor);
  });
}

for (const width of [390, 540, 700, 701, 1280]) {
  for (const descriptions of [false, true]) {
    test(`route selection preserves card geometry at ${width}px, descriptions=${descriptions}`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: 900 });
      const state = await backend(page);
      state.interfaces[2].name = descriptions ? "WireGuard tunnel with a description" : "";
      state.interfaces[3].name = descriptions ? "Long interface description ".repeat(10) : "";
      const longName = Array(12).fill("Long profile name").join(" ");
      state.profiles = [
        { id: "short", name: "VPN", interfaces: ["tun0"], on_unavailable: "blackhole" },
        { id: "long", name: longName, interfaces: ["tun1"], on_unavailable: "blackhole" },
      ];
      state.groups[0].profile = "missing_profile";
      state.subscriptions[0].profile = "missing_profile";
      state.groups.push({ ...state.groups[0], id: "aabbccee", name: "Following group" });
      state.subscriptions.push({
        ...state.subscriptions[0],
        id: "11223355",
        name: "Following subscription",
      });
      await page.goto("/");
      await page.evaluate(() => document.fonts.ready);
      for (const section of ["Groups", "Subscriptions"]) {
        await navigate(page, section);
        const headers = page.locator(
          section === "Groups" ? ".group-header" : ".subscription-header",
        );
        await expect(headers).toHaveCount(2);
        const route = headers.first().locator('[data-select-trigger][aria-label="Route"]');
        const initial = {
          header: await geometry(headers.first()),
          following: await geometry(headers.last()),
          route: await geometry(route),
        };
        const widths = new Map<string, number>();
        for (const value of ["tun0", "tun1", "blackhole", "VPN", longName, "tun2", "tun0"]) {
          await route.click();
          const list = page.getByRole("listbox");
          await expect(list).toBeVisible();
          const popup = await geometry(list);
          expect(popup.x).toBeGreaterThanOrEqual(0);
          expect(popup.x + popup.width).toBeLessThanOrEqual(width);
          const option = page.getByRole("option", {
            name: value.startsWith("tun") ? new RegExp(`^${value}(\\s|$)`) : value,
            exact: !value.startsWith("tun"),
          });
          await option.click();
          await expect(list).not.toBeVisible();
          await expect(route).toContainText(value);
          const current = await geometry(headers.first());
          const following = await geometry(headers.last());
          const selected = await geometry(route);
          expect(selected.height).toBeGreaterThanOrEqual(30);
          expect(selected.height).toBeLessThanOrEqual(33);
          expect(Math.abs(selected.height - initial.route.height)).toBeLessThan(0.5);
          expect(
            Math.abs(current.height - initial.header.height),
            `${section}: ${value}, initial=${initial.header.height}, current=${current.height}`,
          ).toBeLessThan(0.5);
          expect(Math.abs(following.y - initial.following.y)).toBeLessThan(0.5);
          widths.set(value, selected.width);
          // The lower padding is part of the hit target even for a short profile name.
          await route.click({ position: { x: 6, y: selected.height - 3 } });
          await expect(list).toBeVisible();
          await page.keyboard.press("Escape");
          expect(
            await page.evaluate(() => document.documentElement.scrollWidth),
          ).toBeLessThanOrEqual(width);
        }
        expect(widths.get("blackhole")!).toBeGreaterThan(widths.get("VPN")! + 10);
        expect(widths.get(longName)!).toBeGreaterThan(widths.get("VPN")! + 20);
      }
    });
  }

  for (const language of ["en", "ru"]) {
    test(`profile header stays stable through editing and first save at ${width}px in ${language}`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: 900 });
      await page.addInitScript(
        (value) => localStorage.setItem("locale", JSON.stringify({ value })),
        language,
      );
      const state = await backend(page);
      state.interfaces[2].name = "WireGuard tunnel";
      const labels =
        language === "ru"
          ? {
              settings: "Настройки",
              add: "Добавить профиль",
              name: "Название профиля",
              interface: "Добавить интерфейс",
            }
          : {
              settings: "Settings",
              add: "Add profile",
              name: "Profile name",
              interface: "Add interface",
            };
      await page.goto("/");
      await navigate(page, labels.settings);
      await page.getByRole("button", { name: labels.add, exact: true }).click();
      const card = page.locator(".profile");
      await expect(card).toBeVisible();
      await page.evaluate(() => document.fonts.ready);
      const header = card.locator(".profile-header");
      const blank = await geometry(header);
      for (const name of ["VPN", "Long profile name ".repeat(10), "Saved VPN"]) {
        await card.getByLabel(labels.name, { exact: true }).fill(name);
        expect(Math.abs((await geometry(header)).height - blank.height)).toBeLessThan(0.5);
      }
      const picker = card.locator(`[data-select-trigger][aria-label="${labels.interface}"]`);
      for (const name of ["tun0", "tun1", "tun2"]) {
        await picker.click();
        await card.getByRole("option", { name: new RegExp(`^${name}(\\s|$)`) }).click();
        await expect(card.locator(`[data-interface="${name}"]`)).toBeVisible();
        expect(Math.abs((await geometry(header)).height - blank.height)).toBeLessThan(0.5);
      }
      await expect(picker).toBeDisabled();
      const rows = card.locator(".interface:not(.terminal) .interface-row");
      const rowHeights = await rows.evaluateAll((nodes) =>
        nodes.map((node) => node.getBoundingClientRect().height),
      );
      expect(Math.max(...rowHeights) - Math.min(...rowHeights)).toBeLessThan(0.5);
      const filled = await geometry(card);
      const rowPositions = await rows.evaluateAll((nodes) =>
        nodes.map((node) => node.getBoundingClientRect().y),
      );
      await page.locator("#save-settings").click();
      await expect(page.locator("#save-settings")).toBeDisabled();
      await expect(card.locator(".usage svg")).toBeVisible();
      expect(state.profiles[0].interfaces).toEqual(["tun0", "tun1", "tun2"]);
      expect(Math.abs((await geometry(header)).height - blank.height)).toBeLessThan(0.5);
      expect(Math.abs((await geometry(card)).height - filled.height)).toBeLessThan(0.5);
      const savedRows = await rows.evaluateAll((nodes) =>
        nodes.map((node) => node.getBoundingClientRect().y),
      );
      savedRows.forEach((y, index) => expect(Math.abs(y - rowPositions[index])).toBeLessThan(0.5));
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
        width,
      );
    });
  }
}

for (const language of ["en", "ru"]) {
  test(`Save distinguishes blocked changes from clean and valid drafts in ${language}`, async ({
    page,
  }) => {
    await page.addInitScript(
      (value) => localStorage.setItem("locale", JSON.stringify({ value })),
      language,
    );
    await backend(page);
    await page.goto("/");
    const saveLabel = language === "ru" ? "Сохранить изменения" : "Save Changes";
    const blockedLabel =
      language === "ru" ? "Пока нельзя сохранить изменения" : "Can’t save changes yet";
    for (const section of [
      { tab: language === "ru" ? "Группы" : "Groups", id: "save-changes", input: ".group-name" },
      {
        tab: language === "ru" ? "Подписки" : "Subscriptions",
        id: "save-subscriptions",
        input: ".subscription-name",
      },
      {
        tab: language === "ru" ? "Настройки" : "Settings",
        id: "save-settings",
        input: ".profile-name",
      },
    ]) {
      await navigate(page, section.tab);
      const button = page.locator(`#${section.id}`);
      await expect(button).toBeDisabled();
      await expect(button).not.toHaveAttribute("data-save-blocked");
      if (section.id === "save-settings") {
        await page
          .getByRole("button", {
            name: language === "ru" ? "Добавить профиль" : "Add profile",
            exact: true,
          })
          .click();
        const card = page.locator(".profile");
        await card.locator("[data-select-trigger]").click();
        await card.getByRole("option", { name: "tun0", exact: true }).click();
      }
      const tooltip = page.locator('[role="tooltip"]').filter({ has: button });
      const name = page.locator(section.input).first();
      for (const value of ["", "   ", "Valid name", ""]) {
        await name.fill(value);
        await expect(button).toHaveAccessibleName(saveLabel);
        await expect(tooltip).toHaveAttribute(
          "data-value",
          value.trim() ? saveLabel : blockedLabel,
        );
        if (value.trim()) {
          await expect(button).toBeEnabled();
          await expect(button).not.toHaveAttribute("data-save-blocked");
          await expect(button).not.toHaveClass(/inactive/);
        } else {
          await expect(button).toBeDisabled();
          await expect(button).toHaveAttribute("data-save-blocked");
          await expect(button).toHaveCSS("animation-play-state", "running", { pseudo: "before" });
          await expect(button).toHaveCSS("opacity", "1", { pseudo: "before" });
          await expect(button).toHaveClass(/inactive/);
        }
      }
      await tooltip.hover();
      await expect(page.locator("#global-tooltip")).toHaveText(blockedLabel);
      // Keep other pages' drafts valid while checking the next editor.
      await name.fill("Valid name");
    }
  });
}

for (const width of [390, 1280]) {
  test(`new unfinished profiles appear first and receive focus on repeated Add (${width}px)`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const state = await backend(page);
    state.profiles = [
      { id: "existing", name: "Original", interfaces: ["tun0"], on_unavailable: "blackhole" },
    ];
    await page.goto("/");
    await openSettings(page);
    const cards = page.locator(".profile");
    const ids = ["existing"];
    for (const value of ["", "Second draft", "Third draft"]) {
      await page.getByRole("button", { name: "Add profile", exact: true }).click();
      await expect(cards).toHaveCount(ids.length + 1);
      const id = await cards.first().getAttribute("data-profile-id");
      expect(id).toBeTruthy();
      expect(ids).not.toContain(id);
      ids.unshift(id!);
      expect(
        await cards.evaluateAll((nodes) =>
          nodes.map((node) => node.getAttribute("data-profile-id")),
        ),
      ).toEqual(ids);
      const name = cards.first().getByLabel("Profile name", { exact: true });
      await expect(name).toBeFocused();
      await expect(name).toBeInViewport();
      await expect(name).toHaveValue("");
      if (value) await page.keyboard.insertText(value);
      await expect(name).toHaveValue(value);
    }
    const names = page.getByLabel("Profile name", { exact: true });
    for (const [index, value] of ["Third draft", "Second draft", "", "Original"].entries())
      await expect(names.nth(index)).toHaveValue(value);
  });

  for (const language of ["en", "ru"] as const) {
    test(`new profiles focus their required name without tab warnings (${width}px, ${language})`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: 844 });
      const state = await backend(page);
      state.profiles = [
        { id: "existing", name: "Original", interfaces: ["tun0"], on_unavailable: "blackhole" },
      ];
      await page.goto("/");
      await openSettings(page);
      if (language === "ru") await page.locator(".locale button").click();
      const ru = language === "ru";
      const profilesTab = page.getByRole("tab", { name: ru ? "Профили" : "Profiles", exact: true });
      const save = page.locator("#save-settings");
      for (const value of ["First profile", "Second profile"]) {
        await page
          .getByRole("button", { name: ru ? "Добавить профиль" : "Add profile", exact: true })
          .click();
        const draft = page.locator(".profile").first();
        const name = draft.getByLabel(ru ? "Название профиля" : "Profile name", { exact: true });
        await expect(name).toBeFocused();
        await expect(name).toBeInViewport();
        await expect(name).toHaveValue("");
        await page.keyboard.insertText(value);
        await expect(name).toHaveValue(value);
        await page
          .getByRole("tab", { name: ru ? "DNS-запросы" : "DNS queries", exact: true })
          .click();
        await expect(draft).toBeHidden();
        await expect(save).toBeDisabled();
        await expect(profilesTab).not.toHaveAttribute("data-invalid");
        await expect(profilesTab).not.toHaveAttribute("aria-invalid");
        await expect(save).toHaveAccessibleDescription(
          ru ? "Профили: Добавьте хотя бы один интерфейс" : "Profiles: Add at least one interface",
        );
        await profilesTab.click();
        await expect(name).toHaveValue(value);
        await expect(profilesTab).toBeFocused();
        await draft
          .locator(
            `[data-select-trigger][aria-label="${ru ? "Добавить интерфейс" : "Add interface"}"]`,
          )
          .click();
        await page.getByRole("option", { name: "tun1", exact: true }).click();
        await expect(save).toBeEnabled();
      }
      expect(state.writes).toBe(0);
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
        width,
      );
      await save.click();
      await expect.poll(() => state.writes).toBe(1);
      expect(state.profiles.map((profile) => profile.name)).toEqual([
        "Second profile",
        "First profile",
        "Original",
      ]);
    });
  }
}
