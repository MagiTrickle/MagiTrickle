import { strict as assert } from "node:assert";

import {
  cleanProfiles,
  exportWithProfiles,
  moveInterface,
  newProfileId,
  prepareProfileImport,
  profileError,
  routeFromValue,
  routeValue,
} from "../../src/modules/settings/profiles-data";
import type { Group, Profile } from "../../src/types";

const profile = (id = "vpn"): Profile => ({
  id,
  name: "VPN",
  interfaces: ["tun0", "tun1", "tun2"],
  on_unavailable: "blackhole",
});
const group = (p = "vpn"): Group => ({
  id: "11223344",
  name: "GitHub",
  interface: "stale0",
  profile: p,
  color: "#ffffff",
  enable: true,
  rules: [],
});

Deno.test("route selection is typed and leaves direct interface behavior independent", () => {
  assert.equal(routeValue({ interface: "vpn" }), "interface/vpn");
  assert.equal(routeValue({ interface: "tun0", profile: "vpn" }), "profile/vpn");
  assert.deepEqual(routeFromValue("profile/vpn", [profile()]), {
    interface: "tun0",
    profile: "vpn",
  });
  assert.deepEqual(routeFromValue("interface/blackhole", [profile()]), {
    interface: "blackhole",
    profile: undefined,
  });
  assert.equal(routeFromValue("profile/missing", [profile()]), undefined);
});
Deno.test("profile validation rejects ambiguities without imposing a count cap", () => {
  assert.equal(profileError(Array.from({ length: 1024 }, (_, i) => profile(`p_${i}`))), undefined);
  for (const interfaces of [
    [],
    ["tun0", "tun0"],
    ["blackhole"],
    ["eth+"],
    ["bad name"],
    ["abcdefghijklmnop"],
  ]) {
    assert(profileError([{ ...profile(), interfaces }]));
  }
  assert(profileError([profile(), profile()]));
  assert(profileError([{ ...profile(), name: " " }]));
  assert(profileError([{ ...profile(), on_unavailable: "direct" as "blackhole" }]));
});
Deno.test("reordering defines primary and never stores active failover state", () => {
  const p = profile();
  moveInterface(p, 2, 0);
  assert.deepEqual(p.interfaces, ["tun2", "tun0", "tun1"]);
  moveInterface(p, -1, 0);
  moveInterface(p, 0, 9);
  assert.deepEqual(routeFromValue("profile/vpn", [p]), { interface: "tun2", profile: "vpn" });
  const clean = cleanProfiles([{ ...p, name: " VPN ", usage: { groups: 2, subscriptions: 3 } }]);
  assert(!("usage" in clean[0]));
  assert.equal(clean[0].name, "VPN");
  assert.notEqual(clean[0].interfaces, p.interfaces);
});
Deno.test("export includes used definitions once and canonical primary shadows", () => {
  const a = group(),
    b = { ...group(), id: "55667788" };
  const exported = exportWithProfiles([a, b], [profile(), profile("unused")]);
  assert.equal(exported.profiles?.length, 1);
  assert.equal(exported.groups[0].interface, "tun0");
  assert.equal(a.interface, "stale0");
  assert.throws(() => exportWithProfiles([a], []), /Missing/);
  assert(!("profiles" in exportWithProfiles([{ ...a, profile: undefined }], [profile()])));
});
Deno.test("import remaps ID collisions once for every referencing group", () => {
  const existing = { ...profile(), name: "Other behavior", interfaces: ["nwg0"] };
  const result = prepareProfileImport(
    [group(), { ...group(), id: "55667788" }],
    [profile()],
    [existing],
    () => "fresh",
  );
  assert.equal(result.profiles.length, 2);
  assert.deepEqual(
    result.groups.map((g) => g.profile),
    ["fresh", "fresh"],
  );
  assert.deepEqual(
    result.groups.map((g) => g.interface),
    ["tun0", "tun0"],
  );
  assert.equal(existing.interfaces[0], "nwg0");
  assert.equal(
    prepareProfileImport([group()], [profile()], [profile()], () => "fresh").profiles.length,
    1,
  );
  assert.throws(() => prepareProfileImport([group()], [], [], () => "fresh"), /Missing/);
});
Deno.test("profile IDs work without crypto.randomUUID or a secure browser context", () => {
  const ids = Array.from({ length: 1000 }, () => newProfileId());
  assert.equal(new Set(ids).size, ids.length);
  assert(ids.every((id) => /^p_[0-9a-f]{24}$/.test(id)));
});
