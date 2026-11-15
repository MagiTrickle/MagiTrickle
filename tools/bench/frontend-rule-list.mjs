#!/usr/bin/env node
// Native rule-list import diagnostics (API fixtures; no router data or writes).
// Start the same frontend build for each comparison after installing its npm
// dependencies and Playwright browsers:
// node tools/bench/frontend-rule-list.mjs --url http://127.0.0.1:5196 --out /tmp/rules-before.json
// Options: --browser chromium|webkit --executable /path/to/browser --profile
// Optional Chromium --trace-layout also saves a DevTools timeline per phase.
// --trusted-paste uses real clipboard Ctrl+V (Chromium only); default bulk seed
// uses DOM.value + input, avoiding Playwright.fill native insertText overhead.
// --config /path/to/config.mtrickle overrides the existing .build/test-data
// fixture. With neither file, generate the same 100 groups × 1,000 domains.
import { createRequire } from "node:module";
import { readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../../", import.meta.url));
const require = createRequire(`${root}src/frontend/package.json`);
const { chromium, webkit } = require("@playwright/test");
const arg = (name, fallback) => {
  const index = process.argv.indexOf(name);
  return index < 0 ? fallback : process.argv[index + 1];
};
const url = arg("--url", "http://127.0.0.1:5196");
const out = arg("--out", "/tmp/mt-native-rule-list.json");
const engine = arg("--browser", "chromium");
if (!["chromium", "webkit"].includes(engine))
  throw new Error("--browser must be chromium or webkit");
const executablePath = arg("--executable", undefined);
const trustedPaste = process.argv.includes("--trusted-paste");
if (trustedPaste && engine !== "chromium")
  throw new Error("--trusted-paste requires Chromium clipboard permissions");
const configArgument = arg("--config", undefined);
const configPath = configArgument
  ? resolve(configArgument)
  : `${root}.build/test-data/config-100000-rules.mtrickle`;
let patterns;
let configSource;
try {
  const source = JSON.parse(await readFile(configPath, "utf8"));
  patterns = source.groups.flatMap((group) =>
    group.rules.map((rule) => rule.rule),
  );
  configSource = {
    kind: "file",
    path: configPath,
    groups: source.groups.length,
  };
} catch (error) {
  if (configArgument || error.code !== "ENOENT") throw error;
  patterns = Array.from(
    { length: 100000 },
    (_, index) => `rule${String(index + 1).padStart(6, "0")}.example.test`,
  );
  configSource = { kind: "generated", groups: 100, rulesPerGroup: 1000 };
}
if (!patterns.length)
  throw new Error("Benchmark configuration must contain rules");
const text = patterns.join("\n");
const group = {
  id: "a0000001",
  name: "Native rule import",
  color: "#42bdf9",
  interface: "blackhole",
  priority: 300,
  enable: true,
  rules: [],
};
const browser = await (engine === "webkit" ? webkit : chromium).launch({
  headless: true,
  ...(executablePath ? { executablePath } : {}),
});
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
if (trustedPaste)
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
page.setDefaultTimeout(20000);
page.setDefaultNavigationTimeout(20000);
const report = {
  url,
  engine,
  browser: browser.version(),
  configSource,
  ruleCount: patterns.length,
  textBytes: Buffer.byteLength(text),
  bulkInput: trustedPaste
    ? "trusted clipboard Ctrl+V"
    : "DOM value + input event (avoids browser insertText bulk-path overhead)",
  phases: {},
  errors: [],
  writes: [],
};
const save = () => writeFile(out, JSON.stringify(report, null, 2));
const shortError = (error) => {
  const message = error instanceof Error ? error.message : String(error);
  return message.length > 1600
    ? `${message.slice(0, 250)}\n… [error truncated] …\n${message.slice(-1200)}`
    : message;
};
const bounded = (promise, ms = 2000) => {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(
        () => reject(new Error("Diagnostic collection timed out")),
        ms,
      );
    }),
  ]).finally(() => clearTimeout(timer));
};
page.on("pageerror", (error) => report.errors.push(error.message));
await page.route("**/api/v1/**", (route) => {
  const request = route.request();
  if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
  const path = new URL(request.url()).pathname.replace("/api/v1", "");
  if (request.method() !== "GET") report.writes.push(path);
  const fixtures = {
    "/auth": { enabled: false },
    "/groups": { groups: [group] },
    "/subscriptions": { subscriptions: [] },
    "/profiles": { profiles: [] },
    "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
    "/system/update/status": { stage: "idle" },
    "/system/update": { installed_version: "test", can_install: false },
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
  };
  return route.fulfill({ json: fixtures[path] ?? {} });
});
await page.route("https://api.github.com/**", (route) =>
  route.fulfill({ json: [] }),
);
await page.addInitScript(() => {
  const metrics = {
    recording: false,
    pasteEvents: [],
    gaps: [],
    tasks: [],
    taskTurnCount: 0,
    maxTaskGapMs: 0,
    lastTaskTime: 0,
    peakPreviewRows: 0,
    peakRuleNodes: 0,
    longTasksSupported:
      globalThis.PerformanceObserver?.supportedEntryTypes?.includes(
        "longtask",
      ) ?? false,
  };
  globalThis.__nativeRuleMetrics = metrics;
  document.addEventListener(
    "paste",
    (event) => {
      if (metrics.recording)
        metrics.pasteEvents.push({
          trusted: event.isTrusted,
          characters: event.clipboardData?.getData("text/plain").length ?? 0,
        });
    },
    true,
  );
  let previous;
  function frame(now) {
    if (metrics.recording) {
      if (previous !== undefined) metrics.gaps.push(now - previous);
      metrics.peakPreviewRows = Math.max(
        metrics.peakPreviewRows,
        document.querySelectorAll(".results-view .line-row").length,
      );
      metrics.peakRuleNodes = Math.max(
        metrics.peakRuleNodes,
        document.querySelectorAll(".rule").length,
      );
      previous = now;
    } else previous = undefined;
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  // A timer records task turns even when a phase completes between paints.
  // These are diagnostics, not machine-dependent latency assertions.
  setInterval(() => {
    if (!metrics.recording) return;
    const now = performance.now();
    metrics.maxTaskGapMs = Math.max(metrics.maxTaskGapMs, now - metrics.lastTaskTime);
    metrics.lastTaskTime = now;
    metrics.taskTurnCount++;
  }, 0);
  if (metrics.longTasksSupported)
    new PerformanceObserver((list) => {
      if (metrics.recording)
        metrics.tasks.push(...list.getEntries().map((entry) => entry.duration));
    }).observe({ type: "longtask" });
});
const profileEnabled =
  engine === "chromium" && process.argv.includes("--profile");
const traceEnabled =
  engine === "chromium" && process.argv.includes("--trace-layout");
const session =
  profileEnabled || traceEnabled
    ? await page.context().newCDPSession(page)
    : undefined;
report.cpuProfilingSupported = profileEnabled;
report.layoutTracingSupported = traceEnabled;
if (profileEnabled) await session.send("Profiler.enable");
async function stopLayoutTrace(name) {
  const completed = new Promise((resolve) =>
    session.once("Tracing.tracingComplete", resolve),
  );
  await session.send("Tracing.end");
  const { stream } = await bounded(completed);
  let json = "";
  while (true) {
    const part = await bounded(session.send("IO.read", { handle: stream }));
    json += part.base64Encoded
      ? Buffer.from(part.data, "base64").toString("utf8")
      : part.data;
    if (part.eof) break;
  }
  await session.send("IO.close", { handle: stream });
  await writeFile(`${out}.${name}.trace.json`, json);
  const events = JSON.parse(json).traceEvents;
  const summarize = (eventName) => {
    const durations = events
      .filter(
        (event) => event.name === eventName && typeof event.dur === "number",
      )
      .map((event) => event.dur / 1000);
    return {
      count: durations.length,
      totalMs: durations.reduce((sum, value) => sum + value, 0),
      maxMs: Math.max(0, ...durations),
    };
  };
  return {
    layout: summarize("Layout"),
    updateLayoutTree: summarize("UpdateLayoutTree"),
  };
}
async function phase(name, action) {
  report.phases[name] = { status: "running" };
  await save();
  await page.evaluate(() =>
    Object.assign(globalThis.__nativeRuleMetrics, {
      recording: true,
      pasteEvents: [],
      gaps: [],
      tasks: [],
      taskTurnCount: 0,
      maxTaskGapMs: 0,
      lastTaskTime: performance.now(),
      peakPreviewRows: 0,
      peakRuleNodes: 0,
    }),
  );
  if (traceEnabled)
    await session.send("Tracing.start", {
      categories: "devtools.timeline",
      transferMode: "ReturnAsStream",
    });
  if (profileEnabled) await session.send("Profiler.start");
  const start = performance.now();
  let failure;
  try {
    await action();
  } catch (error) {
    failure = error;
  }
  const elapsedMs = performance.now() - start;
  const result = {
    status: failure ? "failed" : "passed",
    elapsedMs,
    ...(failure ? { error: shortError(failure) } : {}),
  };
  report.phases[name] = result;
  // Persist the action result before asking a possibly blocked renderer for
  // diagnostics, so even a native textarea hang leaves a useful report.
  await save();
  try {
    const metrics = await bounded(
      page.evaluate(() => {
        globalThis.__nativeRuleMetrics.recording = false;
        return globalThis.__nativeRuleMetrics;
      }),
    );
    const gaps = metrics.gaps.sort((a, b) => a - b);
    Object.assign(result, {
      pasteEvents: metrics.pasteEvents,
      frameCount: gaps.length,
      taskTurnCount: metrics.taskTurnCount,
      maxTaskGapMs: metrics.maxTaskGapMs,
      maxFrameGapMs: gaps.at(-1) ?? 0,
      p95FrameGapMs: gaps[Math.floor(gaps.length * 0.95)] ?? 0,
      longTasksSupported: metrics.longTasksSupported,
      longTaskCount: metrics.longTasksSupported ? metrics.tasks.length : null,
      longTaskTotalMs: metrics.longTasksSupported
        ? metrics.tasks.reduce((sum, duration) => sum + duration, 0)
        : null,
      maxLongTaskMs: metrics.longTasksSupported
        ? Math.max(0, ...metrics.tasks)
        : null,
      peakPreviewRows: metrics.peakPreviewRows,
      peakRuleNodes: metrics.peakRuleNodes,
    });
    if (profileEnabled) {
      const { profile } = await bounded(session.send("Profiler.stop"));
      await writeFile(`${out}.${name}.cpuprofile`, JSON.stringify(profile));
    }
    if (traceEnabled) result.rendering = await stopLayoutTrace(name);
  } catch (error) {
    result.diagnosticsError = shortError(error);
  }
  await save();
  console.log(name, JSON.stringify(result));
  if (failure) throw failure;
}
try {
  await save();
  await page.goto(url);
  await page.locator("input.group-name").waitFor();
  const collapse = page.locator(".group [data-collapsible-trigger]");
  if ((await collapse.getAttribute("aria-expanded")) === "false")
    await collapse.click();
  await page.locator('[data-value="Import Rule List"] button').click();
  const dialog = page.locator("[data-dialog-content]");
  const textarea = dialog.locator("textarea");
  const preview = dialog.locator(".results-view");
  await phase("pastePreview", async () => {
    if (trustedPaste) {
      await page.evaluate(
        (value) => navigator.clipboard.writeText(value),
        text,
      );
      await textarea.focus();
      await page.keyboard.press("Control+V");
    } else {
      await textarea.evaluate((node, value) => {
        node.focus();
        node.value = value;
        node.dispatchEvent(
          new InputEvent("input", {
            bubbles: true,
            inputType: "insertFromPaste",
            data: null,
          }),
        );
      }, text);
    }
    await textarea.blur();
    await preview.waitFor({ state: "visible" });
    report.previewRows = await preview.locator(".line-row").count();
  });
  await phase("editPreview", async () => {
    const steps = {};
    report.editSteps = steps;
    const step = async (name, action) => {
      const started = performance.now();
      await action();
      steps[name] = performance.now() - started;
    };
    await step("openEditorMs", async () => {
      await preview.click();
      await textarea.waitFor({ state: "visible" });
    });
    await step("homeMs", () => textarea.press("Control+Home"));
    await step("insertCharacterMs", () => page.keyboard.insertText("x"));
    await step("deleteCharacterMs", () => textarea.press("Backspace"));
    await step("blurPreviewMs", async () => {
      await textarea.blur();
      await preview.waitFor({ state: "visible" });
    });
  });
  await phase("submit", async () => {
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await dialog.waitFor({ state: "hidden" });
    await page.locator(".rule").first().waitFor({ state: "visible" });
  });
  const downloading = page.waitForEvent("download");
  await page
    .getByRole("button", { name: "Export Config", exact: true })
    .click();
  const download = await downloading;
  const rules = JSON.parse(await readFile(await download.path(), "utf8"))
    .groups[0].rules;
  report.validation = {
    exportedRules: rules.length,
    uniqueIds: new Set(rules.map((rule) => rule.id)).size,
    orderMatches:
      rules.length === patterns.length &&
      rules.every((rule, index) => rule.rule === patterns[index]),
    noWrites: report.writes.length === 0,
  };
  if (
    rules.length !== patterns.length ||
    report.validation.uniqueIds !== patterns.length ||
    !report.validation.orderMatches ||
    !report.validation.noWrites
  )
    throw new Error("Native import result validation failed");
} catch (error) {
  report.failure = shortError(error);
  process.exitCode = 1;
} finally {
  await save();
  await browser.close();
}
