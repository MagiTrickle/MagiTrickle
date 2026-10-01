import { defineConfig, devices } from "@playwright/test";

// Runs the same tests/e2e suite against a *real* magitrickled-c serving
// the production `dist/` build as its "default" skin, instead of the
// Vite dev server. Unlike playwright.config.ts, there is no webServer
// entry here -- tests/differential/run_e2e_diff.sh starts and stops the
// C daemon itself (mirroring run_http_diff.sh's pattern for the Phase 6
// HTTP contract suite), since the daemon isn't an npm script.
export default defineConfig({
  testDir: "./tests/e2e",
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: 0,
  workers: 1,
  reporter: "list",
  outputDir: "node_modules/.playwright-results-c-backend",
  use: {
    // localhost (not 127.0.0.1) matters: groups.spec.ts's clipboard
    // tests grant permissions for the literal origin
    // "http://localhost:5173", and a permission grant's origin must
    // match the page's origin exactly.
    baseURL: process.env.MT_E2E_BASE_URL || "http://localhost:5173",
    trace: "off",
    screenshot: "off",
    video: "off",
  },
  projects: [
    {
      name: "chromium",
      use: {
        ...devices["Desktop Chrome"],
        // This environment pre-installs a specific Chromium revision at
        // a fixed path rather than the one @playwright/test's version
        // would otherwise auto-download; point at it explicitly instead
        // of running `playwright install`.
        launchOptions: { executablePath: "/opt/pw-browsers/chromium-1194/chrome-linux/chrome" },
      },
    },
  ],
});
