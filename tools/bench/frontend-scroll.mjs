#!/usr/bin/env node
// Production frontend keyboard-scroll diagnostics with deterministic API fixtures.
// Start the same build variant before and after a change, then run:
// node tools/bench/frontend-scroll.mjs --url http://localhost:5197 --out /tmp/scroll.json
// Optional: --browser webkit --executable /path/to/webkit-launcher
// Optional: --scenario panels|rules|mixed to isolate a rendering workload.
// Results include raw rAF positions, row identities and Chromium CPU profiles.
// Linux WebKit is not native macOS Safari; unsupported long-task metrics are null.
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { writeFile } from "node:fs/promises";
const root = fileURLToPath(new URL("../../", import.meta.url));
const require = createRequire(`${root}src/frontend/package.json`);
const { chromium, webkit } = require("@playwright/test");
const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i < 0 ? fallback : process.argv[i + 1];
};
const engine = arg("--browser", "chromium");
if (!["chromium", "webkit"].includes(engine))
  throw new Error("--browser must be chromium or webkit");
const url = arg("--url", "http://localhost:5173");
const out = arg("--out", "/tmp/mt-frontend-scroll.json");
const prefix = out.replace(/\.json$/, "");
const executablePath = arg("--executable", undefined);
const scenario = arg("--scenario", undefined);
const scenarios = scenario ? [scenario] : ["panels", "rules", "mixed"];
if (scenarios.some((item) => !["panels", "rules", "mixed"].includes(item)))
  throw new Error("--scenario must be panels, rules or mixed");
const browser = await (engine === "webkit" ? webkit : chromium).launch({
  headless: true,
  ...(executablePath ? { executablePath } : {}),
});
const results = [];
try {
  for (const kind of scenarios) {
    const page = await browser.newPage({
      viewport: { width: 1280, height: 900 },
    });
    const groups = Array.from(
      { length: kind === "rules" ? 1 : kind === "panels" ? 1000 : 100 },
      (_, i) => ({
        id: (0x10000000 + i).toString(16),
        name: `Group ${i + 1}`,
        color: "#42bdf9",
        interface: "blackhole",
        enable: true,
        priority: 300,
        rules: Array.from(
          { length: kind === "rules" ? 100000 : kind === "panels" ? 0 : 1000 },
          (_, j) => ({
            id: (0x20000000 + i * 100000 + j).toString(16),
            name: "",
            rule: `rule${i}-${j}.example.test`,
            type: "namespace",
            enable: true,
          }),
        ),
      }),
    );
    await page.route("**/api/v1/**", (route) => {
      const p = new URL(route.request().url()).pathname.replace("/api/v1", "");
      const fixtures = {
        "/auth": { enabled: false },
        "/groups": { groups },
        "/subscriptions": { subscriptions: [] },
        "/profiles": { profiles: [] },
        "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
        "/system/update/status": { stage: "idle" },
        "/system/update": { installed_version: "test", can_install: false },
      };
      return route.fulfill({ json: fixtures[p] ?? {} });
    });
    await page.route("https://api.github.com/**", (r) =>
      r.fulfill({ json: [] }),
    );
    await page.addInitScript(() => {
      window.__scrollMetrics = {
        active: false,
        started: 0,
        frames: [],
        tasks: [],
        newRows: 0,
        peakRows: 0,
        longTasksSupported:
          globalThis.PerformanceObserver?.supportedEntryTypes?.includes(
            "longtask",
          ) ?? false,
      };
      let previous = 0;
      const seen = new WeakSet();
      function sample(t) {
        const m = window.__scrollMetrics;
        if (m.active) {
          m.frames.push({
            time: t,
            dt: previous ? t - previous : 0,
            y: scrollY,
          });
          const rows = document.querySelectorAll(".rule");
          m.peakRows = Math.max(m.peakRows, rows.length);
          for (const row of rows)
            if (!seen.has(row)) {
              seen.add(row);
              m.newRows++;
            }
        }
        previous = t;
        requestAnimationFrame(sample);
      }
      requestAnimationFrame(sample);
      try {
        if (window.__scrollMetrics.longTasksSupported)
          new PerformanceObserver((list) => {
            if (window.__scrollMetrics.active)
              window.__scrollMetrics.tasks.push(
                ...list
                  .getEntries()
                  .filter(
                    (entry) =>
                      entry.startTime >= window.__scrollMetrics.started,
                  )
                  .map((entry) => entry.duration),
              );
          }).observe({ type: "longtask" });
      } catch {
        window.__scrollMetrics.longTasksSupported = false;
      }
    });
    await page.goto(url);
    await page.locator("input.group-name").first().waitFor();
    if (kind !== "panels") {
      await page.locator(".group [data-collapsible-trigger]").first().click();
      await page.locator(".rule").first().waitFor();
    }
    await page.evaluate(() => document.activeElement?.blur());
    // Let initial font/size observations finish before measuring navigation.
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(200);
    const cdp =
      engine === "chromium"
        ? await page.context().newCDPSession(page)
        : undefined;
    await cdp?.send("Profiler.enable");
    await cdp?.send("Profiler.start");
    await page.evaluate(() => {
      window.__scrollMetrics.started = performance.now();
      window.__scrollMetrics.active = true;
    });
    await page.keyboard.press("Control+ArrowDown");
    await page.waitForFunction(
      () =>
        Math.abs(
          document.documentElement.scrollHeight - innerHeight - scrollY,
        ) < 1,
      undefined,
      { timeout: 10000, polling: "raf" },
    );
    await page.keyboard.press("Control+ArrowUp");
    await page.waitForFunction(() => scrollY < 1, undefined, {
      timeout: 10000,
      polling: "raf",
    });
    await page.evaluate(() => (window.__scrollMetrics.active = false));
    const profile = cdp ? (await cdp.send("Profiler.stop")).profile : undefined;
    const metrics = await page.evaluate(() => window.__scrollMetrics);
    if (profile)
      await writeFile(`${prefix}.${kind}.cpuprofile`, JSON.stringify(profile));
    // The first sampled interval began before recording was enabled.
    const gaps = metrics.frames
      .slice(1)
      .map((f) => f.dt)
      .sort((a, b) => a - b);
    const moving = metrics.frames.filter(
      (f, i) => i && f.y !== metrics.frames[i - 1].y,
    );
    const hot = (profile?.nodes ?? [])
      .sort((a, b) => (b.hitCount ?? 0) - (a.hitCount ?? 0))
      .slice(0, 12)
      .map((n) => ({
        name: n.callFrame.functionName,
        line: n.callFrame.lineNumber,
        url: n.callFrame.url,
        hits: n.hitCount,
      }));
    const result = {
      kind,
      engine,
      groupCount: groups.length,
      ruleCount: groups.reduce((sum, group) => sum + group.rules.length, 0),
      frames: metrics.frames.length,
      movingFrames: moving.length,
      maxGap: gaps.at(-1),
      p95: gaps[Math.floor(gaps.length * 0.95)],
      longTasksSupported: metrics.longTasksSupported,
      longTasks: metrics.longTasksSupported ? metrics.tasks : null,
      newRows: metrics.newRows,
      peakRows: metrics.peakRows,
      finalY: metrics.frames.at(-1)?.y,
      hot,
    };
    results.push(result);
    console.log(JSON.stringify(result));
    await writeFile(`${prefix}.${kind}.frames.json`, JSON.stringify(metrics));
    await page.close();
  }
  await writeFile(
    out,
    JSON.stringify(
      {
        url,
        engine,
        browser: browser.version(),
        cpuProfilingSupported: engine === "chromium",
        scenarios: results,
      },
      null,
      2,
    ),
  );
} finally {
  await browser.close();
}
