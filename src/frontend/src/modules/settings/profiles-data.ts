import type { Group, Profile } from "../../types";

export type RouteChoice = { interface: string; profile?: string };

/** Keep the UI selection's identity separate from Linux interface names. */
export function routeValue(route: RouteChoice): string {
  return route.profile ? `profile/${route.profile}` : `interface/${route.interface}`;
}

export function routeFromValue(
  value: string,
  profiles: readonly Profile[],
): RouteChoice | undefined {
  if (value.startsWith("interface/")) return { interface: value.slice(10), profile: undefined };
  if (!value.startsWith("profile/")) return;
  const profile = profiles.find((item) => item.id === value.slice(8));
  if (!profile?.interfaces.length) return;
  return { interface: profile.interfaces[0], profile: profile.id };
}

export function cleanProfiles(profiles: readonly Profile[]): Profile[] {
  return profiles.map(({ id, name, interfaces }) => ({
    id,
    name: name.trim(),
    interfaces: [...interfaces],
    on_unavailable: "blackhole",
  }));
}

export function profileError(profiles: readonly Profile[]): string | undefined {
  const ids = new Set<string>();
  for (const profile of profiles) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(profile.id) || ids.has(profile.id))
      return "Invalid profile ID";
    ids.add(profile.id);
    if (
      !profile.name.trim() ||
      new TextEncoder().encode(profile.name).length > 256 ||
      /[\x00-\x1f\x7f]/.test(profile.name)
    ) {
      return "Enter a profile name";
    }
    if (profile.on_unavailable !== undefined && profile.on_unavailable !== "blackhole")
      return "Invalid terminal action";
    if (!profile.interfaces.length) return "Add at least one interface";
    const names = new Set<string>();
    for (const name of profile.interfaces) {
      if (!/^[a-zA-Z0-9_.:-]{1,15}$/.test(name) || [".", "..", "blackhole"].includes(name))
        return "Select an interface";
      if (names.has(name)) return "Interfaces must not repeat";
      names.add(name);
    }
  }
}

export function moveInterface(profile: Profile, from: number, to: number): void {
  if (
    from < 0 ||
    to < 0 ||
    from >= profile.interfaces.length ||
    to >= profile.interfaces.length ||
    from === to
  )
    return;
  const [value] = profile.interfaces.splice(from, 1);
  profile.interfaces.splice(to, 0, value);
}

/** Export definitions exactly once. Never substitute a runtime active link for
 * the configured primary, and never export dangling references silently. */
export function exportWithProfiles(groups: Group[], profiles: readonly Profile[]) {
  const used = new Set(groups.flatMap((group) => (group.profile ? [group.profile] : [])));
  const selected = profiles.filter((profile) => used.has(profile.id));
  if (selected.length !== used.size) throw new Error("Missing routing profile");
  const byId = new Map(selected.map((profile) => [profile.id, profile]));
  const normalized = groups.map((group) =>
    group.profile ? { ...group, interface: byId.get(group.profile)!.interfaces[0] } : group,
  );
  return selected.length
    ? { groups: normalized, profiles: cleanProfiles(selected) }
    : { groups: normalized };
}

/** Remap all imported references together. Name equality is not identity.
 * An ID collision with a different definition makes an independent copy. */
export function prepareProfileImport(
  groups: Group[],
  imported: readonly Profile[],
  current: readonly Profile[],
  newId: () => string,
) {
  const error = profileError(imported);
  if (error) throw new Error(error);
  const next = cleanProfiles(current);
  const ids = new Set(next.map((profile) => profile.id));
  const mapping = new Map<string, string>();
  for (const profile of cleanProfiles(imported)) {
    const existing = next.find((item) => item.id === profile.id);
    const oldId = profile.id;
    if (existing && JSON.stringify(existing) === JSON.stringify(profile)) {
      mapping.set(oldId, oldId);
      continue;
    }
    if (existing) {
      do {
        profile.id = newId();
      } while (ids.has(profile.id));
    }
    ids.add(profile.id);
    mapping.set(oldId, profile.id);
    next.push(profile);
  }
  const mapped = groups.map((group) => {
    if (!group.profile) return { ...group };
    const id = mapping.get(group.profile) ?? group.profile;
    const definition = next.find((profile) => profile.id === id);
    if (!definition) throw new Error("Missing routing profile");
    return { ...group, profile: id, interface: definition.interfaces[0] };
  });
  return { profiles: next, groups: mapped };
}

export function newProfileId(): string {
  const bytes = new Uint8Array(12);
  crypto.getRandomValues(bytes);
  return "p_" + [...bytes].map((value) => value.toString(16).padStart(2, "0")).join("");
}
