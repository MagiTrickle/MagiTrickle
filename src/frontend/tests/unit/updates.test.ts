import assert from "node:assert/strict";

import {
  CACHE_TTL,
  candidate,
  compareVersions,
  parseBuildInfo,
  parseRelease,
  parseStatus,
  parseVersion,
  pickRelease,
  RELEASE_API,
  RELEASE_WEB,
  ReleaseClient,
  type BuildInfo,
  type Release,
} from "../../src/modules/updates/releases.ts";

const info: BuildInfo = {
  installed_version: "0.8.2.2",
  installed_revision: 1,
  asset_suffix: "entware_aarch64-3.10_kn.ipk",
  can_install: true,
  reason: "",
};
function release(tag = "0.8.3"): Release {
  const name = `mt-c_${tag}-1_${info.asset_suffix}`;
  return {
    id: 123,
    tag_name: tag,
    name: "Release",
    body: "Notes",
    published_at: "2026-10-10T12:00:00Z",
    draft: false,
    prerelease: false,
    assets: [
      {
        name,
        browser_download_url: `${RELEASE_WEB}/download/${tag}/${name}`,
        state: "uploaded",
        size: 300000,
        digest: `sha256:${"a".repeat(64)}`,
      },
    ],
  };
}
function storage() {
  const entries = new Map<string, string>();
  return {
    getItem: (key: string) => entries.get(key) ?? null,
    setItem: (key: string, value: string) => {
      entries.set(key, value);
    },
  };
}

Deno.test("numeric versions, package revisions and snapshots never imply a downgrade", () => {
  assert.equal(compareVersions("0.8.10", "0.8.9"), 1);
  assert.equal(compareVersions("0.8.3", "0.8.2.2"), 1);
  assert.equal(compareVersions("0.8.3", "0.8.3", 1, 2), -1);
  assert.equal(compareVersions("0.8.3-rev2", "0.8.3", 1, 1), 1);
  assert.equal(compareVersions("0.8.3", "0.8.3~git20261010120000.abcdef0"), 1);
  assert.equal(compareVersions("0.8.3", "0.8.4~git20261010120000.abcdef0"), -1);
  assert.equal(parseVersion("0.8.4_pre20261010140000")?.development, true);
  assert.equal(compareVersions("0.8.4", "0.8.4_pre20261010140000"), 1);
  assert.equal(compareVersions("0.8.3", "0.8.4_pre20261010140000"), -1);
  assert.equal(
    compareVersions("0.8.4_pre20261010140000", "0.8.4~git20261010140000.abcdef0"),
    0,
  );
  for (const invalid of [
    "0.8.4_pre2026101014000",
    "0.8.4_pre202610101400000",
    "0.8.4_pre20261010140000.abcdef0",
    "0.8.4_pre20261010140000-rev1",
  ]) {
    assert.equal(parseVersion(invalid), null);
  }
  assert.equal(compareVersions("0.8.3", "unattached"), null);
  assert.equal(compareVersions("0.8.3/../../evil", "0.8.2"), null);
  assert.equal(compareVersions("0.8.3-rev0", "0.8.2"), null);
});

Deno.test(
  "stable excludes prereleases; preview selects latest published rather than array order",
  () => {
    const newer = { ...release(), prerelease: true };
    const old = { ...release("0.8.2.2"), published_at: "2026-10-01T12:00:00Z" };
    assert.equal(pickRelease([newer], false), null);
    assert.equal(pickRelease([old, newer], false)?.tag_name, "0.8.2.2");
    assert.equal(pickRelease([old, newer], true)?.tag_name, "0.8.3");
    assert.equal(pickRelease([{ ...newer, draft: true }, old], true)?.tag_name, "0.8.2.2");
    assert.equal(pickRelease([], true), null);
  },
);

Deno.test("exact asset, checksum and official URL required", () => {
  assert.ok(candidate(release(), info).asset);
  for (const patch of [
    { name: "mt-c_0.8.3-1_entware_aarch64-3.10.ipk" },
    { browser_download_url: "https://evil.example/package.ipk" },
    { state: "new" },
    { digest: null },
    { digest: `sha256:${"z".repeat(64)}` },
    { size: 65 * 1024 * 1024 },
  ]) {
    const item = release();
    Object.assign(item.assets[0], patch);
    assert.equal(candidate(item, info).asset, null);
  }
  const duplicate = release();
  duplicate.assets.push({ ...duplicate.assets[0] });
  assert.equal(candidate(duplicate, info).reason, "Ambiguous package assets");
});

Deno.test("OpenWrt ABI/firmware and APK revision spelling are explicit", () => {
  const build = { ...info, asset_suffix: "openwrt-25.12.5_aarch64_cortex-a53.apk" };
  for (const revision of ["1", "r1"]) {
    const item = release();
    item.assets[0].name = `mt-c_0.8.3-${revision}_${build.asset_suffix}`;
    item.assets[0].browser_download_url = `${RELEASE_WEB}/download/0.8.3/${item.assets[0].name}`;
    assert.ok(candidate(item, build).asset);
    assert.equal(
      candidate(item, { ...build, asset_suffix: "openwrt-24.10.4_aarch64_cortex-a53.ipk" }).asset,
      null,
    );
  }
});

Deno.test("installation capability is not inferred from finding a release", () => {
  assert.equal(
    candidate(release(), {
      ...info,
      can_install: false,
      reason: "Sign in as root to install updates",
    }).reason,
    "Sign in as root to install updates",
  );
  assert.equal(candidate(release(), { ...info, installed_version: "0.9.0" }).newer, false);
  assert.equal(candidate(release(), { ...info, asset_suffix: "" }).asset, null);
});

Deno.test("runtime responses are validated, not merely cast", () => {
  assert.deepEqual(parseBuildInfo(info), info);
  assert.throws(() => parseBuildInfo({ ...info, installed_revision: 0 }));
  assert.throws(() => parseStatus({ stage: "installing" }));
  assert.equal(parseStatus({ stage: "queued", job_id: "a".repeat(32) }).stage, "queued");
  assert.throws(() => parseRelease({ ...release(), assets: null }));
});

Deno.test("browser checks cache for one hour and never sends router credentials", async () => {
  const cache = storage();
  let count = 0;
  const request: typeof fetch = async (url, options) => {
    count++;
    assert.equal(String(url), `${RELEASE_API}?per_page=100`);
    assert.equal(options?.credentials, "omit");
    assert.equal(new Headers(options?.headers).has("Authorization"), false);
    return new Response(JSON.stringify([release()]), { headers: { ETag: '"first"' } });
  };
  const client = new ReleaseClient(cache, request);
  await client.latest(false, false);
  await client.latest(false, false);
  assert.equal(count, 1);
  await client.latest(false, true);
  assert.equal(count, 2);
});

Deno.test("expired cache uses ETag and accepts 304 without reading a JSON body", async () => {
  const cache = storage();
  cache.setItem(
    "mt-c.releases.v2",
    JSON.stringify({
      releases: [release()],
      checkedAt: Date.now() - CACHE_TTL - 1000,
      etag: '"old"',
    }),
  );
  const client = new ReleaseClient(cache, async (_url, options) => {
    assert.equal(new Headers(options?.headers).get("If-None-Match"), '"old"');
    return new Response(null, { status: 304 });
  });
  assert.equal((await client.latest(false, false)).release?.tag_name, "0.8.3");
});

Deno.test(
  "404 clears stale release and ETag; manual checks cannot bypass rate backoff",
  async () => {
    const cache = storage();
    cache.setItem(
      "mt-c.releases.v2",
      JSON.stringify({ releases: [release()], checkedAt: 0, etag: '"old"' }),
    );
    const missing = new ReleaseClient(cache, async () => new Response(null, { status: 404 }));
    const result = await missing.latest(false, true);
    assert.equal(result.release, null);
    assert.equal(result.etag, "");
    let calls = 0;
    const limited = new ReleaseClient(null, async () => {
      calls++;
      return new Response(null, { status: 403, headers: { "Retry-After": "60" } });
    });
    await assert.rejects(limited.latest(false, true), /rate limit/);
    await assert.rejects(limited.latest(false, true), /rate limit/);
    assert.equal(calls, 1);
  },
);

Deno.test(
  "overlapping channels share a request and toggling selects from one cached list",
  async () => {
    const cache = storage();
    let calls = 0;
    const stable = release("0.8.2.2");
    const preview = { ...release(), prerelease: true, published_at: "2026-10-11T12:00:00Z" };
    const client = new ReleaseClient(cache, async () => {
      calls++;
      await Promise.resolve();
      return new Response(JSON.stringify([stable, preview]));
    });
    const [stableResult, previewResult] = await Promise.all([
      client.latest(false, false),
      client.latest(true, false),
    ]);
    assert.equal(stableResult.release?.tag_name, "0.8.2.2");
    assert.equal(previewResult.release?.tag_name, "0.8.3");
    assert.equal(client.cached(false)?.release?.tag_name, "0.8.2.2");
    assert.equal(client.cached(true)?.release?.tag_name, "0.8.3");
    await client.latest(true, false);
    await client.latest(false, false);
    assert.equal(calls, 1);
  },
);

Deno.test("blocked storage is optional and failed network does not become up-to-date", async () => {
  const denied = {
    getItem: () => {
      throw new Error("blocked");
    },
    setItem: () => {
      throw new Error("blocked");
    },
  };
  const client = new ReleaseClient(denied, async () => new Response(JSON.stringify([release()])));
  assert.equal((await client.latest(false, false)).release?.tag_name, "0.8.3");
  assert.equal(client.cached(true)?.release?.tag_name, "0.8.3");
  const offline = new ReleaseClient(null, async () => {
    throw new TypeError("offline");
  });
  await assert.rejects(offline.latest(false, false), /offline/);
});

Deno.test("the shared list selects by publication date beyond the former 20-item window", async () => {
  const older = { ...release("2.0.0"), published_at: "2026-10-01T00:00:00Z" };
  const later = { ...release("1.1.0"), published_at: "2026-10-03T00:00:00Z" };
  const preview = {
    ...release("3.0.0"),
    prerelease: true,
    published_at: "2026-10-04T00:00:00Z",
  };
  assert.equal(pickRelease([older, later, preview], false)?.tag_name, "1.1.0");
  assert.equal(pickRelease([older, later, preview], true)?.tag_name, "3.0.0");

  const list = Array.from({ length: 30 }, () => ({ ...older, prerelease: true }));
  list.push(later);
  let calls = 0;
  const client = new ReleaseClient(null, async (url) => {
    assert.equal(String(url), `${RELEASE_API}?per_page=100`);
    calls++;
    return new Response(JSON.stringify(list));
  });
  assert.equal((await client.latest(false, false)).release?.tag_name, "1.1.0");
  assert.equal((await client.latest(true, false)).release?.tag_name, "1.1.0");
  assert.equal(calls, 1);
});
