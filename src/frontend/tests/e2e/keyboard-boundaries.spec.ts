import { expect, test, type Page } from "@playwright/test";

import type { Group } from "../../src/types";

async function loadLargeList(page: Page, panels: boolean) {
  const rulesPerGroup = panels ? 100 : 100000;
  const groups: Group[] = Array.from({ length: panels ? 1000 : 1 }, (_, group) => ({
    id: (0x10000000 + group).toString(16),
    name: `Boundary group ${group + 1}`,
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: Array.from({ length: rulesPerGroup }, (_, rule) => ({
      id: (0x20000000 + group * rulesPerGroup + rule).toString(16),
      name: "",
      rule: `item-${group}-${rule}.example.test`,
      type: "namespace",
      enable: true,
    })),
  }));
  await page.route("**/api/v1/**", (route) => {
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
  await expect(page.locator("input.group-name").first()).toHaveValue(groups[0].name);
  if (!panels) {
    await page.locator(".group [data-collapsible-trigger]").click();
    await expect(page.locator(".rule").first()).toBeVisible();
  }
  await page.evaluate(async () => {
    (document.activeElement as HTMLElement | null)?.blur();
    // Let the first painted rows supply their measurements before the test
    // captures a target offset; DOM visibility alone precedes ResizeObserver.
    await new Promise<void>((resolve) =>
      requestAnimationFrame(() => requestAnimationFrame(() => resolve())),
    );
  });
  return groups;
}

async function documentEdge(page: Page) {
  return page.evaluate(() => ({
    top: scrollY,
    bottom: Math.max(0, document.documentElement.scrollHeight - innerHeight - scrollY),
  }));
}

for (const width of [1280, 390]) {
  for (const panels of [true, false]) {
    test(`one boundary shortcut settles at both edges of ${panels ? "1000 panels" : "100k rules"} at ${width}px`, async ({
      page,
    }) => {
      test.setTimeout(90000);
      await page.setViewportSize({ width, height: 844 });
      const groups = await loadLargeList(page, panels);
      const last = groups.at(-1)!;
      for (const [down, up] of [
        ["Control+ArrowDown", "Control+ArrowUp"],
        ["Meta+ArrowDown", "Meta+ArrowUp"],
        ["End", "Home"],
      ]) {
        await page.keyboard.press(down);
        await expect
          .poll(async () => (await documentEdge(page)).bottom, { message: `${down}: document end` })
          .toBeLessThan(1);
        if (panels) {
          await expect(
            page.locator(`.group[data-uuid="${last.id}"] input.group-name`),
          ).toBeVisible();
        } else {
          await expect(page.locator(`.rule[data-uuid="${last.rules.at(-1)!.id}"]`)).toBeVisible();
        }
        // A native jump can initially hit zero gap, then lose the edge when
        // nested virtual rows are measured. Check after that settling too.
        await page.waitForTimeout(250);
        expect((await documentEdge(page)).bottom).toBeLessThan(1);
        await page.keyboard.press(up);
        await expect
          .poll(async () => (await documentEdge(page)).top, { message: `${up}: document start` })
          .toBeLessThan(1);
        await expect(page.locator("input.group-name").first()).toHaveValue(groups[0].name);
        await page.waitForTimeout(250);
        expect((await documentEdge(page)).top).toBeLessThan(1);
      }
    });
  }
}

test("boundary shortcuts preserve input caret and editable or modal keyboard handling", async ({
  page,
}) => {
  await loadLargeList(page, true);
  const input = page.locator("input.group-name").first();
  await input.focus();
  await input.press("Control+Home");
  expect(await input.evaluate((node: HTMLInputElement) => node.selectionStart)).toBe(0);
  await input.press("Control+End");
  expect(await input.evaluate((node: HTMLInputElement) => node.selectionStart)).toBe(
    "Boundary group 1".length,
  );
  expect((await documentEdge(page)).top).toBeLessThan(1);

  const results = await page.evaluate(() => {
    const fixture = document.createElement("div");
    fixture.style.cssText = "position:fixed;top:0;left:0;z-index:10000";
    fixture.innerHTML = `<input value="Caret example"><textarea>Caret example</textarea>
      <div contenteditable="true">Caret example</div><select><option>First</option><option>Second</option></select>
      <div role="dialog"><button>Modal control</button></div>
      <div role="menu"><button role="menuitem">Popup control</button></div>`;
    document.body.append(fixture);
    const results: { element: string; shortcut: string; prevented: boolean }[] = [];
    try {
      for (const target of fixture.querySelectorAll<HTMLElement>(
        "input, textarea, [contenteditable], select, button",
      )) {
        target.focus({ preventScroll: true });
        for (const [key, ctrlKey, metaKey] of [
          ["ArrowDown", true, false],
          ["ArrowUp", false, true],
          ["End", false, false],
          ["Home", true, false],
        ] as const) {
          const event = new KeyboardEvent("keydown", {
            key,
            ctrlKey,
            metaKey,
            bubbles: true,
            cancelable: true,
          });
          target.dispatchEvent(event);
          results.push({
            element: target.outerHTML,
            shortcut: `${ctrlKey ? "Control+" : metaKey ? "Meta+" : ""}${key}`,
            prevented: event.defaultPrevented,
          });
        }
      }
    } finally {
      fixture.remove();
    }
    return results;
  });
  expect(results.filter((result) => result.prevented)).toEqual([]);
  await page.waitForTimeout(250);
  expect((await documentEdge(page)).top).toBeLessThan(1);
});

test("manual wheel input releases a boundary jump while virtual heights settle", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await loadLargeList(page, true);
  await page.keyboard.press("Control+ArrowDown");
  await expect.poll(async () => (await documentEdge(page)).bottom).toBeLessThan(1);
  await page.mouse.move(200, 400);
  await page.mouse.wheel(0, -1100);
  await expect.poll(async () => (await documentEdge(page)).bottom).toBeGreaterThan(200);
  await page.waitForTimeout(400);
  expect((await documentEdge(page)).bottom).toBeGreaterThan(200);
  // Deliver another navigation request and cancel before it completes.
  // Synthetic wheel input has no native scroll, so move the viewport
  // explicitly after delivering the cancellation event.
  await page.evaluate(
    () =>
      new Promise<void>((resolve) => {
        window.dispatchEvent(
          new KeyboardEvent("keydown", {
            key: "ArrowDown",
            ctrlKey: true,
            bubbles: true,
            cancelable: true,
          }),
        );
        requestAnimationFrame(() => {
          window.dispatchEvent(new WheelEvent("wheel", { deltaY: -1100 }));
          window.scrollBy(0, -1100);
          resolve();
        });
      }),
  );
  await page.waitForTimeout(300);
  expect((await documentEdge(page)).bottom).toBeGreaterThan(200);
});

test("wheel input after scrollend cancels the pending edge check", async ({ page }) => {
  await loadLargeList(page, true);
  await page.evaluate(
    () =>
      new Promise<void>((resolve) => {
        window.dispatchEvent(
          new KeyboardEvent("keydown", {
            key: "ArrowDown",
            metaKey: true,
            bubbles: true,
            cancelable: true,
          }),
        );
        // The boundary controller queues its layout check first. Simulate the
        // user's next wheel before that check can reclaim the requested edge.
        function onEnd(event: Event) {
          if (event.target !== document) return;
          document.removeEventListener("scrollend", onEnd);
          window.dispatchEvent(new WheelEvent("wheel", { deltaY: -1100 }));
          window.scrollBy(0, -1100);
          resolve();
        }
        document.addEventListener("scrollend", onEnd);
      }),
  );
  await expect.poll(async () => (await documentEdge(page)).bottom).toBeGreaterThan(200);
  await page.waitForTimeout(400);
  expect((await documentEdge(page)).bottom).toBeGreaterThan(200);
});

test("boundary shortcuts leave the priority popover buttons in control", async ({ page }) => {
  await loadLargeList(page, true);
  await page.locator(".priority-trigger").first().click();
  const decrease = page.getByRole("button", { name: "Decrease priority", exact: true });
  await expect(decrease).toBeFocused();
  const before = await documentEdge(page);
  for (const shortcut of ["Control+ArrowDown", "Meta+ArrowDown", "End"]) {
    await decrease.press(shortcut);
    await expect(decrease).toBeFocused();
    await expect(page.locator(".priority-popover")).toBeVisible();
    await page.waitForTimeout(100);
    expect((await documentEdge(page)).top).toBe(before.top);
  }
  await decrease.press("Enter");
  await expect(page.getByRole("spinbutton")).toHaveValue("299");
  await decrease.press("Escape");
  await expect(page.locator(".priority-popover")).toBeHidden();
});

for (const panels of [true, false]) {
  test(`native boundary scrolling is not interrupted by ${panels ? "panel" : "100k rule"} measurements`, async ({
    page,
  }) => {
    test.setTimeout(90000);
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await loadLargeList(page, panels);
    type Trace = {
      positions: { top: number; bottom: number }[];
      calls: string[];
      prevented: boolean[];
    };
    type TraceWindow = Window & { finishScrollTrace?: () => Trace };
    for (const [shortcut, edge, alias] of [
      ["Control+ArrowDown", "bottom", true],
      ["Meta+ArrowUp", "top", true],
      ["End", "bottom", false],
      ["Home", "top", false],
    ] as const) {
      await page.evaluate(() => {
        const trace: Trace = { positions: [], calls: [], prevented: [] };
        const scrollTo = window.scrollTo;
        const scrollBy = window.scrollBy;
        window.scrollTo = new Proxy(scrollTo, {
          apply(target, receiver, argumentsList) {
            trace.calls.push("scrollTo");
            return Reflect.apply(target, receiver, argumentsList);
          },
        });
        window.scrollBy = new Proxy(scrollBy, {
          apply(target, receiver, argumentsList) {
            trace.calls.push("scrollBy");
            return Reflect.apply(target, receiver, argumentsList);
          },
        });
        const onScroll = () =>
          trace.positions.push({
            top: scrollY,
            bottom: document.documentElement.scrollHeight - innerHeight - scrollY,
          });
        const onKey = (event: KeyboardEvent) => trace.prevented.push(event.defaultPrevented);
        window.addEventListener("scroll", onScroll, { passive: true });
        window.addEventListener("keydown", onKey);
        (window as TraceWindow).finishScrollTrace = () => {
          window.scrollTo = scrollTo;
          window.scrollBy = scrollBy;
          window.removeEventListener("scroll", onScroll);
          window.removeEventListener("keydown", onKey);
          return trace;
        };
      });
      await page.keyboard.press(shortcut);
      await expect
        .poll(async () => (await documentEdge(page))[edge], { message: `${shortcut}: ${edge}` })
        .toBeLessThan(1);
      // Check through the following layout frames as well as the first arrival.
      await page.evaluate(
        () =>
          new Promise<void>((resolve) => {
            let remaining = 12;
            function sample() {
              if (--remaining) requestAnimationFrame(sample);
              else resolve();
            }
            requestAnimationFrame(sample);
          }),
      );
      expect((await documentEdge(page))[edge]).toBeLessThan(1);
      const trace = await page.evaluate(() => {
        const state = window as TraceWindow;
        const trace = state.finishScrollTrace!();
        delete state.finishScrollTrace;
        return trace;
      });
      // One native scroll starts an alias; measurement callbacks must not write
      // scroll offsets and cancel its animation. Home/End need no scripted move.
      expect(trace.calls).toEqual(alias ? ["scrollTo"] : []);
      if (alias) {
        expect(
          trace.positions.some((position) => position.top > 100 && position.bottom > 100),
        ).toBe(true);
      } else {
        expect(trace.prevented).toEqual([false]);
      }
    }
  });
}

test("reduced motion makes boundary aliases immediate while preserving the exact edge", async ({
  page,
}) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await loadLargeList(page, true);
  for (const [shortcut, edge] of [
    ["Control+ArrowDown", "bottom"],
    ["Meta+ArrowUp", "top"],
  ] as const) {
    await page.keyboard.press(shortcut);
    expect((await documentEdge(page))[edge]).toBeLessThan(1);
    await page.waitForTimeout(250);
    expect((await documentEdge(page))[edge]).toBeLessThan(1);
  }
});
