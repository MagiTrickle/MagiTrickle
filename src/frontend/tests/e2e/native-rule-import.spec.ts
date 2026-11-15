import { readFile } from "node:fs/promises";
import { expect, test, type Locator, type Page } from "@playwright/test";

import type { RuleListResponse } from "../../src/modules/groups/rule-list-parser";
import type { Group } from "../../src/types";

async function editor(page: Page) {
  const group: Group = {
    id: "11223344",
    name: "Rule import",
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: [],
  };
  const writes: string[] = [];
  await page.route("**/api/v1/**", (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() !== "GET") writes.push(path);
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups: [group] },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/config/snapshot": {
        revision: "a".repeat(64),
        config: {
          configVersion: "test",
          app: {},
          groups: [group],
          subscriptions: [],
          profiles: [],
        },
      },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
  await page.goto("/");
  await expect(page.locator("input.group-name")).toHaveValue(group.name);
  const collapse = page.locator(".group [data-collapsible-trigger]");
  if ((await collapse.getAttribute("aria-expanded")) === "false") await collapse.click();
  const trigger = page.locator('[data-value="Import Rule List"] button');
  const open = async () => {
    await trigger.click();
    const dialog = page.locator("[data-dialog-content]");
    await expect(dialog).toBeVisible();
    return dialog;
  };
  return { open, writes };
}

// Playwright.fill uses native insertText for the complete value; on a plain
// textarea that path itself stalls at 100k lines, independently of this app.
// Seed the bulk value atomically, then exercise real keyboard edits below.
async function seedBulkText(textarea: Locator, text: string) {
  await textarea.evaluate((node: HTMLTextAreaElement, value) => {
    node.focus();
    node.value = value;
    node.dispatchEvent(
      new InputEvent("input", { bubbles: true, inputType: "insertFromPaste", data: null }),
    );
  }, text);
}

const largeText = () =>
  Array.from({ length: 100000 }, (_, index) => `item${index}.example.test`).join("\n");

type StagingProbe = {
  armed: boolean;
  timerRan: boolean;
  actedWhileApplying: boolean;
  actionTaken: boolean;
  rowsBeforeAction: number;
  taskDelayMs: number;
};
type ProbeWindow = Window & { nativeImportProbe?: StagingProbe };

async function interruptNextRuleStaging(page: Page, action: "close" | "edit") {
  await page.addInitScript((action) => {
    const probe: StagingProbe = {
      armed: true,
      timerRan: false,
      actedWhileApplying: false,
      actionTaken: false,
      rowsBeforeAction: -1,
      taskDelayMs: 0,
    };
    (window as ProbeWindow).nativeImportProbe = probe;
    const NativeWorker = window.Worker;
    window.Worker = class extends NativeWorker {
      constructor(url: string | URL, options?: WorkerOptions) {
        super(url, options);
        if (!String(url).includes("rule-list-parser.worker")) return;
        this.addEventListener("message", (event: MessageEvent<RuleListResponse>) => {
          if (!probe.armed || event.data.type !== "chunk" || !event.data.done) return;
          probe.armed = false;
          const receivedAt = performance.now();
          // The final response enters preparation through its promise before
          // this task runs. Acting immediately after clicking Import could
          // instead happen during transfer, before the store revision is read.
          setTimeout(() => {
            probe.timerRan = true;
            probe.taskDelayMs = performance.now() - receivedAt;
            const dialog = document.querySelector('[data-dialog-content][data-state="open"]');
            const submit = dialog?.querySelector<HTMLButtonElement>('button[type="submit"]');
            probe.actedWhileApplying = Boolean(submit?.disabled);
            probe.rowsBeforeAction = document.querySelectorAll(".group .rule").length;
            if (!probe.actedWhileApplying) return;
            if (action === "close") {
              const close = dialog?.querySelector<HTMLButtonElement>("[data-dialog-close]");
              if (!close) return;
              close.click();
            } else {
              const name = document.querySelector<HTMLInputElement>("input.group-name");
              if (!name) return;
              name.value = "Changed during import";
              name.dispatchEvent(new Event("input", { bubbles: true }));
            }
            probe.actionTaken = true;
          }, 0);
        });
      }
    };
  }, action);
  return () => page.evaluate(() => (window as ProbeWindow).nativeImportProbe!);
}

test("100k native rule import keeps the editor buffer, limits preview DOM and imports edits in full", async ({
  page,
}, testInfo) => {
  test.setTimeout(120000);
  const { open, writes } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  const originalEditor = await textarea.elementHandle();
  await seedBulkText(textarea, largeText());
  await textarea.blur();
  const preview = dialog.locator(".results-view");
  await expect(preview).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  expect(await preview.locator(".line-row").count()).toBeLessThan(80);
  const documentY = await page.evaluate(() => scrollY);
  await preview.evaluate((node) => node.scrollTo(0, node.scrollHeight));
  await expect(
    preview.locator(".line-row").filter({ hasText: "item499.example.test" }),
  ).toBeVisible();
  expect(await page.evaluate(() => scrollY)).toBe(documentY);
  await preview.click();
  await expect(textarea).toBeFocused();
  await textarea.blur();
  await expect(preview.locator(".line-row").first()).toContainText("item0.example.test");
  await preview.click();
  await expect(textarea).toBeFocused();
  expect(
    await originalEditor!.evaluate(
      (node) => node === document.querySelector("[data-dialog-content] textarea"),
    ),
  ).toBe(true);
  await expect(textarea).toHaveAttribute("spellcheck", "false");
  await expect(textarea).toHaveAttribute("autocorrect", "off");
  await textarea.press("Control+Home");
  await page.keyboard.insertText("edited.");
  await expect(textarea).toHaveValue(/^edited\.item0\.example\.test/);
  await textarea.blur();
  await expect(preview).toBeVisible();
  await textarea.evaluate((node: HTMLTextAreaElement) => {
    const value = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value")!;
    node.dataset.connectedClears = "0";
    Object.defineProperty(node, "value", {
      configurable: true,
      get: () => value.get!.call(node),
      set: (next: string) => {
        // Closing must remove the native editor before clearing its bulk
        // buffer. Replacing 100k live lines also rebuilds native text layout.
        if (!next && node.isConnected)
          node.dataset.connectedClears = String(Number(node.dataset.connectedClears) + 1);
        value.set!.call(node, next);
      },
    });
  });
  type ApplyMetrics = {
    elapsedMs: number;
    frameCount: number;
    maxFrameGapMs: number;
    p95FrameGapMs: number;
    taskTurnCount: number;
    maxTaskGapMs: number;
    longTasksSupported: boolean;
    longTaskCount: number | null;
    maxLongTaskMs: number | null;
    longTaskTotalMs: number | null;
  };
  type MetricsWindow = Window & { finishImportMetrics?: () => ApplyMetrics };
  const submitBox = await dialog.getByRole("button", { name: "Import", exact: true }).boundingBox();
  expect(submitBox).not.toBeNull();
  await page.evaluate(() => {
    const started = performance.now();
    const gaps: number[] = [];
    const tasks: number[] = [];
    let previousFrame = started;
    let previousTask = started;
    let taskTurnCount = 0;
    let maxTaskGapMs = 0;
    let frame = 0;
    function sample(now: number) {
      gaps.push(now - previousFrame);
      previousFrame = now;
      frame = requestAnimationFrame(sample);
    }
    frame = requestAnimationFrame(sample);
    const timer = setInterval(() => {
      const now = performance.now();
      maxTaskGapMs = Math.max(maxTaskGapMs, now - previousTask);
      previousTask = now;
      taskTurnCount++;
    }, 0);
    const supported =
      globalThis.PerformanceObserver?.supportedEntryTypes?.includes("longtask") ?? false;
    const collect = (entries: PerformanceEntry[]) => {
      for (const entry of entries) if (entry.startTime >= started) tasks.push(entry.duration);
    };
    const observer = supported
      ? new PerformanceObserver((list) => collect(list.getEntries()))
      : undefined;
    observer?.observe({ type: "longtask" });
    (window as MetricsWindow).finishImportMetrics = () => {
      cancelAnimationFrame(frame);
      clearInterval(timer);
      if (observer) {
        collect(observer.takeRecords());
        observer.disconnect();
      }
      const now = performance.now();
      gaps.sort((a, b) => a - b);
      return {
        elapsedMs: now - started,
        frameCount: gaps.length,
        maxFrameGapMs: Math.max(0, ...gaps),
        p95FrameGapMs: gaps[Math.floor(gaps.length * 0.95)] ?? 0,
        taskTurnCount,
        maxTaskGapMs: Math.max(maxTaskGapMs, now - previousTask),
        longTasksSupported: supported,
        longTaskCount: supported ? tasks.length : null,
        maxLongTaskMs: supported ? Math.max(0, ...tasks) : null,
        longTaskTotalMs: supported ? tasks.reduce((sum, duration) => sum + duration, 0) : null,
      };
    };
  });
  await page.mouse.click(submitBox!.x + submitBox!.width / 2, submitBox!.y + submitBox!.height / 2);
  // Polling locator assertions adds accessibility-snapshot work over the still
  // mounted 100k-line textarea. Keep test-runner diagnostics outside this interval.
  await page.waitForFunction(() => {
    const save = document.querySelector<HTMLButtonElement>("#save-changes");
    return Boolean(
      !document.querySelector("[data-dialog-content]") &&
      save &&
      !save.disabled &&
      document.querySelector(".group .rule"),
    );
  });
  const metrics = await page.evaluate(() => {
    const state = window as MetricsWindow;
    const result = state.finishImportMetrics!();
    delete state.finishImportMetrics;
    return result;
  });
  await expect(dialog).toBeHidden();
  expect(await originalEditor!.evaluate((node) => node.dataset.connectedClears)).toBe("0");
  await expect(page.locator("#save-changes")).toBeEnabled();
  await expect(page.locator(".rule").first()).toBeVisible();
  await testInfo.attach("native-100k-apply-metrics.json", {
    body: JSON.stringify(metrics, null, 2),
    contentType: "application/json",
  });
  // Record successful Apply separately from paste/edit and the full export.
  // These diagnostics have no hardware-dependent pass/fail threshold.
  console.log("native-100k-apply-metrics", JSON.stringify(metrics));
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const path = await (await downloading).path();
  const exported = JSON.parse(await readFile(path!, "utf8")) as { groups: Group[] };
  const rules = exported.groups[0].rules;
  expect(rules).toHaveLength(100000);
  expect(rules[0].rule).toBe("edited.item0.example.test");
  expect(rules.at(-1)!.rule).toBe("item99999.example.test");
  expect(new Set(rules.map((rule) => rule.id)).size).toBe(100000);
  expect(writes).toEqual([]);
});

test("closing a pending native rule preview cannot replace the next session", async ({ page }) => {
  const { open, writes } = await editor(page);
  let dialog = await open();
  await seedBulkText(dialog.locator("textarea"), largeText());
  await dialog.locator("textarea").blur();
  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
  dialog = await open();
  await dialog.locator("textarea").fill("fresh.example.test\n[\nfresh.example.test");
  await dialog.locator("textarea").blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".line-row")).toHaveCount(3);
  await expect(dialog.locator(".count-badge", { hasText: "namespace" })).toContainText("2");
  await expect(dialog.locator(".count-badge", { hasText: "INVALID" })).toContainText("1");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".pattern-input")).toHaveCount(1);
  await expect(page.locator(".pattern-input")).toHaveValue("fresh.example.test");
  expect(writes).toEqual([]);
});

test("changing the type while editing invalidates a cached native preview", async ({ page }) => {
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await textarea.fill("first.example.test\nsecond.example.test");
  await textarea.blur();
  await expect(dialog.locator(".count-badge", { hasText: "namespace" })).toContainText("2");
  await dialog.locator(".results-view").click();
  await expect(textarea).toBeFocused();
  await dialog.locator("[data-select-trigger]").click();
  await page.getByRole("option", { name: "Domain", exact: true }).click();
  await expect(dialog.locator(".count-badge", { hasText: "domain" })).toContainText("2");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".rule")).toHaveCount(2);
  await expect(page.locator(".rule").first()).toContainText("Domain");
});

test("reopening during the dialog exit starts a fresh editor without a late reset", async ({
  page,
}) => {
  const { open, writes } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await expect(dialog).toHaveCSS("opacity", "1");
  await textarea.fill("old.example.test");
  const oldEditor = await textarea.elementHandle();
  const closing = await page.evaluate(async () => {
    const dialog = document.querySelector("[data-dialog-content]")!;
    document.querySelector<HTMLButtonElement>("[data-dialog-close]")!.click();
    // Flush the controlled closed state before reopening during the outro.
    // Two synchronous clicks collapse false -> true in Svelte's batch and do
    // not represent a second input event reaching the closing dialog.
    await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
    const closing = { connected: dialog.isConnected, state: dialog.getAttribute("data-state") };
    document.querySelector<HTMLButtonElement>('[data-value="Import Rule List"] button')!.click();
    return closing;
  });
  expect(closing).toEqual({ connected: true, state: "closed" });
  await expect(dialog).toHaveAttribute("data-state", "open");
  await expect(dialog).toBeVisible();
  await expect(textarea).toHaveValue("");
  expect(await oldEditor!.evaluate((node) => node.isConnected)).toBe(false);
  await textarea.fill("new.example.test");
  await textarea.blur();
  await expect(dialog.locator(".count-val")).toHaveText("1");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".pattern-input")).toHaveValue("new.example.test");
  expect(writes).toEqual([]);
});

test("a failed native bulk apply retains its parsed input for retry", async ({ page }) => {
  test.setTimeout(120000);
  const readProbe = await interruptNextRuleStaging(page, "edit");
  const { open, writes } = await editor(page);
  const dialog = await open();
  await seedBulkText(dialog.locator("textarea"), largeText());
  await dialog.locator("textarea").blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  const submit = dialog.getByRole("button", { name: "Import", exact: true });
  await submit.click();
  await expect.poll(async () => (await readProbe()).timerRan).toBe(true);
  const probe = await readProbe();
  expect(probe.actedWhileApplying).toBe(true);
  expect(probe.actionTaken).toBe(true);
  expect(probe.rowsBeforeAction).toBe(0);
  await expect(page.locator("input.group-name")).toHaveValue("Changed during import");
  await expect(submit).toBeEnabled();
  await expect(dialog).toBeVisible();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  await dialog.locator(".results-view").click();
  await expect(dialog.locator("textarea")).toBeFocused();
  await expect(dialog.locator("textarea")).toHaveValue(/^item0\.example\.test/);
  expect(
    await dialog
      .locator("textarea")
      .evaluate((node: HTMLTextAreaElement) => node.value.split("\n").length),
  ).toBe(100000);
  await expect(page.locator(".group .rule")).toHaveCount(0);
  expect(writes).toEqual([]);
});

test("a failed worker transfer can retry unchanged native rule text", async ({ page }) => {
  type FailureWindow = Window & { nativeApplyFailed?: boolean };
  await page.addInitScript(() => {
    const state = window as FailureWindow;
    state.nativeApplyFailed = false;
    const NativeWorker = window.Worker;
    window.Worker = class extends NativeWorker {
      constructor(url: string | URL, options?: WorkerOptions) {
        super(url, options);
        if (!String(url).includes("rule-list-parser.worker")) return;
        this.addEventListener("message", (event: MessageEvent<RuleListResponse>) => {
          if (state.nativeApplyFailed || event.data.type !== "chunk") return;
          state.nativeApplyFailed = true;
          event.stopImmediatePropagation();
          this.dispatchEvent(
            new ErrorEvent("error", { message: "Simulated worker failure", cancelable: true }),
          );
        });
      }
    };
  });
  const { open, writes } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  const originalEditor = await textarea.elementHandle();
  const text = "retry-one.example.test\nretry-two.example.test";
  await textarea.fill(text);
  await textarea.blur();
  await expect(dialog.locator(".count-val")).toHaveText("2");
  const submit = dialog.getByRole("button", { name: "Import", exact: true });
  await submit.click();
  await expect
    .poll(() => page.evaluate(() => (window as FailureWindow).nativeApplyFailed))
    .toBe(true);
  await expect(submit).toBeEnabled();
  await expect(dialog).toBeVisible();
  await expect(textarea).toHaveValue(text);
  expect(
    await originalEditor!.evaluate(
      (node) => node === document.querySelector("[data-dialog-content] textarea"),
    ),
  ).toBe(true);
  await expect(page.locator(".group .rule")).toHaveCount(0);
  await expect(page.locator("#save-changes")).toBeDisabled();

  await submit.click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".pattern-input")).toHaveCount(2);
  await expect(page.locator(".pattern-input").first()).toHaveValue("retry-one.example.test");
  await expect(page.locator(".pattern-input").last()).toHaveValue("retry-two.example.test");
  expect(writes).toEqual([]);
});

test("100k native import yields to cancellation during client staging without publishing partial rules", async ({
  page,
}, testInfo) => {
  test.setTimeout(120000);
  const readProbe = await interruptNextRuleStaging(page, "close");

  const { open, writes } = await editor(page);
  let dialog = await open();
  await seedBulkText(dialog.locator("textarea"), largeText());
  await dialog.locator("textarea").blur();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect.poll(async () => (await readProbe()).timerRan).toBe(true);
  const probe = await readProbe();
  await testInfo.attach("native-import-cancellation.json", {
    body: JSON.stringify(probe, null, 2),
    contentType: "application/json",
  });
  expect(probe.actedWhileApplying).toBe(true);
  expect(probe.actionTaken).toBe(true);
  expect(probe.rowsBeforeAction).toBe(0);
  await expect(dialog).toBeHidden();
  await expect(page.locator("#save-changes")).toBeDisabled();

  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const path = await (await downloading).path();
  const exported = JSON.parse(await readFile(path!, "utf8")) as { groups: Group[] };
  expect(exported.groups[0].rules).toEqual([]);

  dialog = await open();
  await dialog.locator("textarea").fill("fresh-one.example.test\nfresh-two.example.test");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".pattern-input")).toHaveCount(2);
  await expect(page.locator(".pattern-input").first()).toHaveValue("fresh-one.example.test");
  await expect(page.locator(".pattern-input").last()).toHaveValue("fresh-two.example.test");
  expect(writes).toEqual([]);
});

test("trusted 100k paste retains native Undo and Redo after returning from preview", async ({
  page,
  browserName,
}) => {
  test.skip(browserName !== "chromium", "Clipboard permission automation is available in Chromium");
  test.setTimeout(120000);
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await page.evaluate((text) => navigator.clipboard.writeText(text), largeText());
  await textarea.focus();
  await page.keyboard.press("Control+V");
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  await dialog.locator(".results-view").click();
  await expect(textarea).toBeFocused();
  await textarea.press("Control+Z");
  await expect(textarea).toHaveValue("");
  await textarea.press("Control+Shift+Z");
  await expect(textarea).toHaveValue(/^item0\.example\.test/);
  await textarea.blur();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
});

test("typing while a bulk preview is pending cancels it and keeps the latest editor text", async ({
  page,
}) => {
  test.setTimeout(120000);
  let releaseFirstWorker!: () => void;
  const held = new Promise<void>((resolve) => {
    releaseFirstWorker = resolve;
  });
  let first = true;
  await page.route("**/rule-list-parser.worker*", async (route) => {
    if (first) {
      first = false;
      await held;
    }
    try {
      await route.continue();
    } catch (error) {
      if (!(error instanceof Error) || !/closed|cancell?ed|already handled/i.test(error.message))
        throw error;
    }
  });
  try {
    const { open } = await editor(page);
    const dialog = await open();
    const textarea = dialog.locator("textarea");
    await seedBulkText(textarea, largeText());
    await textarea.dispatchEvent("paste");
    await expect(dialog.locator(".parsing-overlay")).toBeVisible();
    await expect.poll(() => first).toBe(false);
    await expect(textarea).toBeEnabled();
    await expect(textarea).toBeFocused();
    await textarea.press("Control+Home");
    await page.keyboard.insertText("latest.");
    await expect(dialog.locator(".parsing-overlay")).toBeHidden();
    await expect(textarea).toBeFocused();
    await expect(textarea).toHaveValue(/^latest\.item0\.example\.test/);
    releaseFirstWorker();
    await textarea.blur();
    await expect(dialog.locator(".results-view")).toBeVisible();
    await expect(dialog.locator(".results-view .line-row").first()).toContainText(
      "latest.item0.example.test",
    );
    await expect(dialog.locator(".count-val")).toHaveText("100000");
  } finally {
    releaseFirstWorker();
  }
});

test("continued typing cancels the scheduled automatic paste preview", async ({ page }) => {
  await page.clock.install();
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await textarea.fill("pasted.example.test");
  await page.clock.pauseAt(await page.evaluate(() => Date.now() + 1000));
  await textarea.dispatchEvent("paste");
  await textarea.press("End");
  await page.keyboard.insertText("x");
  await page.clock.runFor(100);
  await expect(dialog.locator(".parsing-overlay")).toBeHidden();
  await expect(dialog.locator(".results-view")).toBeHidden();
  await expect(textarea).toBeFocused();
  await expect(textarea).toHaveValue("pasted.example.testx");
  await page.clock.resume();
  await textarea.blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".line-row")).toContainText("pasted.example.testx");
});
