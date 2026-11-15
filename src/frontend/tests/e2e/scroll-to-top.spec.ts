import { expect, test, type Locator, type Page } from "@playwright/test";

async function loadGroups(page: Page, manyPanels: boolean) {
  const groups = Array.from({ length: manyPanels ? 1000 : 1 }, (_, index) => ({
    id: (0x10000000 + index).toString(16),
    name: `Scrolling group ${index + 1}`,
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: Array.from({ length: manyPanels ? 0 : 100000 }, (_, rule) => ({
      id: (0x20000000 + rule).toString(16),
      name: "",
      rule: `item-${rule}.example.test`,
      type: "namespace",
      enable: true,
    })),
  }));
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().method() === "OPTIONS") return route.fulfill({ status: 204 });
    const path = new URL(route.request().url()).pathname.replace("/api/v1", "");
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
  await page.goto("/");
  await expect(page.locator("input.group-name").first()).toHaveValue("Scrolling group 1");
  if (!manyPanels) {
    await page.locator(".group [data-collapsible-trigger]").click();
    await expect(page.locator(".rule").first()).toBeVisible();
  }
}

for (const manyPanels of [true, false]) {
  test(`Back to top repeatedly reaches the document start through virtual ${manyPanels ? "panels" : "100k rules"}`, async ({
    page,
  }) => {
    test.setTimeout(60000);
    await loadGroups(page, manyPanels);
    const button = page.getByRole("button", { name: "Back to top", exact: true });
    for (const fraction of [0.9, 0.5, 0.95]) {
      await page.evaluate(
        (fraction) => window.scrollTo(0, document.documentElement.scrollHeight * fraction),
        fraction,
      );
      await expect.poll(() => page.evaluate(() => window.scrollY)).toBeGreaterThan(1000);
      await expect(button).toBeVisible();
      await button.click();
      await expect
        .poll(() => page.evaluate(() => window.scrollY), { timeout: 8000 })
        .toBeLessThan(1);
      await expect(button).toBeHidden();
      await expect(page.locator("input.group-name").first()).toHaveValue("Scrolling group 1");
    }
  });
}

test("Back to top respects reduced motion", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await loadGroups(page, true);
  await page.evaluate(() => window.scrollTo(0, 10000));
  const button = page.getByRole("button", { name: "Back to top", exact: true });
  await expect(button).toBeVisible();
  await button.click();
  expect(await page.evaluate(() => window.scrollY)).toBe(0);
  await expect(button).toBeHidden();
});

async function settledScrollY(page: Page) {
  return page.evaluate(
    () =>
      new Promise<number>((resolve, reject) => {
        let last = scrollY;
        let stable = 0;
        let frame = 0;
        const timeout = setTimeout(() => {
          cancelAnimationFrame(frame);
          reject(new Error("Wheel scrolling did not settle"));
        }, 8000);
        function sample() {
          stable = Math.abs(scrollY - last) < 1 ? stable + 1 : 0;
          last = scrollY;
          if (stable < 12) frame = requestAnimationFrame(sample);
          else {
            clearTimeout(timeout);
            resolve(scrollY);
          }
        }
        frame = requestAnimationFrame(sample);
      }),
  );
}

async function measureWheelInterruption(page: Page, button: Locator) {
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight * 0.9));
  await expect(button).toBeVisible();
  await page.mouse.move(600, 400);
  // Observe actual browser scroll events: Playwright's fake JS clock does not
  // control native smooth scrolling on the compositor.
  const during = await button.evaluate(
    (node) =>
      new Promise<number>((resolve, reject) => {
        const start = scrollY;
        const timeout = setTimeout(() => {
          window.removeEventListener("scroll", onScroll);
          reject(new Error("Back to top did not begin smooth scrolling"));
        }, 8000);
        function onScroll() {
          if (scrollY >= start - 100 || scrollY <= 1000) return;
          clearTimeout(timeout);
          window.removeEventListener("scroll", onScroll);
          resolve(scrollY);
        }
        window.addEventListener("scroll", onScroll, { passive: true });
        (node as HTMLButtonElement).click();
      }),
  );
  expect(during).toBeGreaterThan(1000);
  const wheelPosition = page.evaluate(
    () =>
      new Promise<number>((resolve) => {
        window.addEventListener("wheel", () => resolve(scrollY), { once: true, passive: true });
      }),
  );
  await page.mouse.wheel(0, 800);
  const canceledAt = await wheelPosition;
  const firstSettled = await settledScrollY(page);
  await page.mouse.wheel(0, 800);
  const nextSettled = await settledScrollY(page);
  return {
    during,
    canceledAt,
    firstSettled,
    firstDelta: firstSettled - canceledAt,
    nextSettled,
    nextDelta: nextSettled - firstSettled,
  };
}

test("manual wheel input stops an ongoing native Back to top animation", async ({ page }) => {
  test.setTimeout(60000);
  await loadGroups(page, true);
  const height = await page.evaluate(() => document.documentElement.scrollHeight);
  const button = page.getByRole("button", { name: "Back to top", exact: true });
  const app = await measureWheelInterruption(page, button);
  await expect(button).toBeVisible();

  // Chromium may consume the first wheel while activating a JS cancellation
  // position. Compare against raw native scrolling on an equally tall document,
  // then require the next trusted wheel to move both pages normally.
  const reference = await page.context().newPage();
  try {
    const viewport = page.viewportSize();
    if (viewport) await reference.setViewportSize(viewport);
    await reference.setContent(`<!doctype html>
      <style>
        html { scrollbar-gutter: stable; }
        body { margin: 0; height: ${height}px; }
        button { position: fixed; right: 16px; bottom: 16px; }
      </style>
      <button type="button">Back to top</button>`);
    await reference.getByRole("button", { name: "Back to top", exact: true }).evaluate((node) => {
      node.addEventListener("click", () => {
        window.addEventListener(
          "wheel",
          () => window.scrollTo({ top: window.scrollY, behavior: "instant" }),
          { once: true, passive: false },
        );
        window.scrollTo({ top: 0, behavior: "smooth" });
      });
    });
    const native = await measureWheelInterruption(
      reference,
      reference.getByRole("button", { name: "Back to top", exact: true }),
    );
    console.log(JSON.stringify({ nativeWheelInterruption: { control: native, app } }));

    expect(native.firstSettled).toBeGreaterThanOrEqual(native.canceledAt - 1);
    expect(app.firstSettled).toBeGreaterThanOrEqual(app.canceledAt - 1);
    expect(Math.abs(app.firstDelta - native.firstDelta)).toBeLessThanOrEqual(1);
    expect(native.nextDelta).toBeGreaterThan(100);
    expect(app.nextDelta).toBeGreaterThan(100);
  } finally {
    await reference.close();
  }
});
