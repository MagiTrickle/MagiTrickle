import { expect, test } from "@playwright/test";

for (const kind of ["groups", "subscriptions"] as const) {
  test(`${kind}: corner selection, bulk interface, clear and delete`, async ({ page, context }) => {
    const items = ["First", "Second", "Third"].map((name, i) => ({
      id: `item-${i}`,
      name,
      interface: "eth0",
      enable: true,
      color: "#7755aa",
      rules: [{ id: `rule-${i}`, rule: `${i}.example.com`, type: "domain", enable: true }],
      url: `https://example.com/${i}`,
      interval: 86400,
      lastUpdate: 0,
    }));
    let saved: typeof items = [];
    const deleted: string[] = [];
    await page.route("**/auth", (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/system/interfaces**", (route) =>
      route.fulfill({
        json: {
          interfaces: [
            { id: "eth0", name: "" },
            { id: "wlan0", name: "Wi-Fi" },
          ],
        },
      }),
    );
    await page.route("**/groups?with_rules=true", (route) =>
      route.fulfill({ json: { groups: kind === "groups" ? items : [] } }),
    );
    await page.route(
      kind === "groups" ? "**/groups?save=true" : "**/subscriptions",
      async (route) => {
        if (route.request().method() === "PUT") saved = route.request().postDataJSON()[kind];
        await route.fulfill({ json: { [kind]: items } });
      },
    );
    await page.route("**/subscriptions/*", async (route) => {
      if (route.request().method() !== "DELETE") return route.fallback();
      deleted.push(route.request().url().split("/").at(-1)!);
      await route.fulfill({ json: { status: "ok" } });
    });
    await context.grantPermissions(["clipboard-read", "clipboard-write"]);
    await page.goto("/");
    if (kind === "subscriptions") await page.getByRole("tab", { name: "Subscriptions" }).click();
    const frames = page.locator(".selection-frame");
    await expect(frames).toHaveCount(3);
    // Hovering must not leave painted corners or select items.
    for (const frame of await frames.all()) {
      const tail = frame.locator(".selection-tail");
      await tail.hover({ position: { x: 8, y: 8 } });
      await expect(frame.locator(".tail-surface")).toHaveCSS("visibility", "visible");
      await page.mouse.move(1, 1);
      await expect(frame.locator(".tail-surface")).toHaveCSS("visibility", "hidden");
      await expect(tail).toHaveAttribute("aria-pressed", "false");
    }
    const firstTail = frames.first().locator(".selection-tail");
    const firstSurface = frames.first().locator(".tail-surface");
    await firstTail.click({ position: { x: 8, y: 8 } });
    await page.mouse.move(1, 1);
    await expect(firstSurface).toHaveCSS("visibility", "visible");
    await firstTail.click({ position: { x: 8, y: 8 } });
    await page.mouse.move(1, 1);
    await expect(firstTail).toBeFocused();
    await expect(firstTail).toHaveAttribute("aria-pressed", "false");
    await expect(firstSurface).toHaveCSS("visibility", "hidden");
    // Hidden graphics must not remove the button from keyboard navigation.
    await page.keyboard.press("Tab");
    await page.keyboard.press("Shift+Tab");
    await expect(firstTail).toBeFocused();
    await expect(firstSurface).toHaveCSS("visibility", "visible");
    await page.keyboard.press("Space");
    await expect(firstTail).toHaveAttribute("aria-pressed", "true");
    await page.keyboard.press("Space");
    await page.keyboard.press("Tab");
    await expect(firstSurface).toHaveCSS("visibility", "hidden");
    await frames.nth(0).hover();
    await page.getByRole("button", { name: "Select item: First", exact: true }).click();
    await frames.nth(2).hover();
    await page.getByRole("button", { name: "Select item: Third", exact: true }).click();
    const bar = page.getByRole("region", { name: "Bulk actions" });
    await expect(bar.getByRole("status")).toContainText("2 selected");
    await bar.getByRole("button", { name: "Copy to Clipboard" }).click();
    await expect
      .poll(() => page.evaluate(() => navigator.clipboard.readText()))
      .toBe("0.example.com\n2.example.com");
    await expect(frames).toHaveCount(3);
    await expect(page.locator(".selection-frame.selected")).toHaveCount(2);
    await bar.getByRole("button", { name: "Interface", exact: true }).click();
    const interfacePopover = page.getByRole("dialog", { name: "Change interface" });
    await expect(
      interfacePopover.getByRole("button", { name: "Apply", exact: true }),
    ).toBeDisabled();
    const initialHeight = await bar.evaluate((node) => node.getBoundingClientRect().height);
    const initialSelectWidth = await interfacePopover
      .getByRole("button", { name: "Choose interface" })
      .evaluate((node) => node.getBoundingClientRect().width);
    const initialCardHeights = await frames.evaluateAll((nodes) =>
      nodes.map((node) => node.getBoundingClientRect().height),
    );
    await interfacePopover.getByRole("button", { name: "Choose interface" }).click();
    await page.getByRole("option", { name: "wlan0" }).click();
    await expect
      .poll(() => bar.evaluate((node) => node.getBoundingClientRect().height))
      .toBe(initialHeight);
    await expect
      .poll(() =>
        interfacePopover
          .getByRole("button", { name: "Choose interface" })
          .evaluate((node) => node.getBoundingClientRect().width),
      )
      .not.toBe(initialSelectWidth);
    await interfacePopover.getByRole("button", { name: "Apply", exact: true }).click();
    await expect(interfacePopover).toHaveCount(0);
    await expect
      .poll(() =>
        frames.evaluateAll((nodes) => nodes.map((node) => node.getBoundingClientRect().height)),
      )
      .toEqual(initialCardHeights);
    await page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions").click();
    await expect
      .poll(() => saved.map((item) => item.interface))
      .toEqual(["wlan0", "eth0", "wlan0"]);
    await bar.getByRole("button", { name: "Clear selection" }).click();
    await expect(bar).toHaveCount(0);
    await expect(page.locator(".selection-frame.selected")).toHaveCount(0);
    await frames.nth(1).hover();
    await page.getByRole("button", { name: "Select item: Second", exact: true }).click();
    await bar.getByRole("button", { name: "Select all" }).click();
    await expect(bar.getByRole("status")).toContainText("3 selected");
    await bar.getByRole("button", { name: "State" }).click();
    await page.getByRole("menuitem", { name: "Disable selected" }).click();
    await page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions").click();
    await expect.poll(() => saved.every((item) => !item.enable)).toBe(true);
    await page.setViewportSize({ width: 390, height: 844 });
    const bounds = await bar.boundingBox();
    expect(bounds!.x).toBeGreaterThanOrEqual(0);
    expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(390);
    await bar.getByRole("button", { name: "Interface", exact: true }).click();
    await interfacePopover.getByRole("button", { name: "Choose interface" }).click();
    await page.getByRole("option", { name: "wlan0" }).click();
    const mobileHeight = await bar.evaluate((node) => node.getBoundingClientRect().height);
    await interfacePopover.getByRole("button", { name: "Choose interface" }).click();
    await page.getByRole("option", { name: "eth0", exact: true }).click();
    await expect
      .poll(() => bar.evaluate((node) => node.getBoundingClientRect().height))
      .toBe(mobileHeight);
    await page.keyboard.press("Escape");
    await expect(interfacePopover).toHaveCount(0);
    await expect(bar).toBeVisible();
    page.once("dialog", (dialog) => dialog.accept());
    await bar.getByRole("button", { name: "Delete selected", exact: true }).click();
    await expect(frames).toHaveCount(0);
    await expect(bar).toHaveCount(0);
    if (kind === "subscriptions") expect(deleted).toEqual(items.map((item) => item.id));
  });
}
