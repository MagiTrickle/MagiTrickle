import { expect, test, type Page } from "@playwright/test";

type Subscription = {
  id: string;
  name: string;
  interface: string;
  enable: boolean;
  url: string;
  lastUpdate: number;
  interval: number;
  rules: { enable: boolean; id: string; rule: string; type: string }[];
};

const makeSubscription = (id: string, url: string, name = `Subscription ${id}`): Subscription => ({
  id,
  name,
  interface: "eth0",
  enable: true,
  url,
  lastUpdate: 1700000000,
  interval: 86400,
  rules: [{ enable: true, id: `${id.slice(0, 4)}aaaa`, rule: `${id}.example.com`, type: "domain" }],
});

async function setupSubscriptions(page: Page, data: Subscription[]) {
  let subscriptions = structuredClone(data);
  let savedBody: { subscriptions: Subscription[] } | null = null;
  let syncBody: { url?: string } | null = null;

  await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
  await page.route("**/groups?with_rules=true", async (route) =>
    route.fulfill({ json: { groups: [] } }),
  );
  await page.route("**/interfaces", async (route) =>
    route.fulfill({ json: { interfaces: ["eth0", "wlan0"] } }),
  );
  await page.route("**/subscriptions", async (route) => {
    if (route.request().method() === "PUT") {
      savedBody = route.request().postDataJSON();
      subscriptions = structuredClone(savedBody?.subscriptions ?? []);
      await route.fulfill({ json: { status: "ok" } });
      return;
    }

    await route.fulfill({ json: { subscriptions } });
  });
  await page.route("**/subscriptions/*/sync", async (route) => {
    syncBody = route.request().postDataJSON();
    const id = route.request().url().split("/").at(-2);
    const subscription = subscriptions.find((item) => item.id === id);
    if (!subscription) {
      await route.fulfill({ status: 404, json: { error: "subscription not found" } });
      return;
    }

    subscription.url = syncBody?.url ?? subscription.url;
    subscription.rules = [
      { enable: true, id: "87654321", rule: "synced.example.com", type: "domain" },
    ];
    subscription.lastUpdate = 1800000000;

    await route.fulfill({
      json: {
        rules: subscription.rules,
        lastUpdate: subscription.lastUpdate,
        url: subscription.url,
      },
    });
  });

  await page.goto("/");
  await page.getByRole("tab", { name: "Subscriptions" }).click();
  await expect(page.locator(".subscription-panel")).toHaveCount(data.length);

  return {
    get savedBody() {
      return savedBody;
    },
    get syncBody() {
      return syncBody;
    },
  };
}

test.describe("Subscriptions", () => {
  test("edits an existing subscription URL and saves it", async ({ page }) => {
    const state = await setupSubscriptions(page, [
      makeSubscription("a1b2c3d4", "https://old.example/list.txt"),
    ]);

    const urlInput = page.locator(".subscription-url-input").first();
    await expect(urlInput).toHaveValue("https://old.example/list.txt");

    await urlInput.fill(" https://new.example/list.txt ");

    const saveButton = page.locator("#save-subscriptions");
    await expect(saveButton).not.toHaveClass(/inactive/);
    await saveButton.click();

    await expect(page.getByText("Saved")).toBeVisible();
    expect(state.savedBody?.subscriptions[0].url).toBe("https://new.example/list.txt");
  });

  test("prevents saving duplicate subscription URLs", async ({ page }) => {
    await setupSubscriptions(page, [
      makeSubscription("a1b2c3d4", "https://one.example/list.txt", "One"),
      makeSubscription("b1c2d3e4", "https://two.example/list.txt", "Two"),
    ]);

    const urlInputs = page.locator(".subscription-url-input");
    await urlInputs.first().fill("https://two.example/list.txt");

    await expect(urlInputs.first()).toHaveClass(/invalid/);
    await expect(page.locator(".url-error")).toHaveCount(0);
    await expect(page.locator("#save-subscriptions")).toHaveClass(/inactive/);
  });

  test("syncs with the edited unsaved subscription URL", async ({ page }) => {
    const state = await setupSubscriptions(page, [
      makeSubscription("a1b2c3d4", "https://old.example/list.txt"),
    ]);

    await page.locator(".subscription-url-input").first().fill(" https://new.example/list.txt ");
    await page.locator(".action.sync button").first().click();

    expect(state.syncBody?.url).toBe("https://new.example/list.txt");
    await expect(page.locator(".subscription-url-input").first()).toHaveValue(
      "https://new.example/list.txt",
    );
    await expect(page.locator("#save-subscriptions")).toHaveClass(/inactive/);
    await expect(page.locator(".subscription-rule")).toContainText("synced.example.com");
  });
});
