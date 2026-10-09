import { parse } from "valibot";

import { cleanProfiles, profileError } from "../modules/settings/profiles-data";
import { ProfileSchema, type Profile } from "../types";
import { fetcher } from "../utils/fetcher";
import { HttpError } from "../utils/http-error";

// Drafts survive tab changes. Refreshing server definitions/usage must not
// discard edits or the pending disk-write acknowledgement.
export const profiles = $state({
  list: [] as Profile[],
  draft: [] as Profile[],
  baseline: "[]",
  loaded: false,
  loading: false,
  failed: false,
  saving: false,
  persistencePending: false,
});
export function profilesDirty() {
  return (
    JSON.stringify(cleanProfiles(profiles.draft)) !== profiles.baseline ||
    profiles.persistencePending
  );
}
function decodeProfiles(data: { profiles: unknown[] }): Profile[] {
  if (!Array.isArray(data?.profiles)) throw new Error("Invalid profiles response");
  const result = data.profiles.map((profile) => parse(ProfileSchema, profile));
  const error = profileError(result);
  if (error) throw new Error(error);
  return result;
}
function accept(list: Profile[], resetDraft: boolean) {
  profiles.list = list;
  profiles.loaded = true;
  profiles.failed = false;
  if (resetDraft) {
    profiles.draft = list.map((profile) => ({ ...profile, interfaces: [...profile.interfaces] }));
    profiles.baseline = JSON.stringify(cleanProfiles(list));
  }
}
let pendingFetch: Promise<void> | undefined;
let generation = 0;
export function fetchProfiles(options: { fresh?: boolean } = {}): Promise<void> {
  if (pendingFetch) {
    // A request started before a consumer was saved may contain obsolete
    // usage. An explicit refresh must follow it, not merely share its result.
    return options.fresh ? pendingFetch.then(() => fetchProfiles()) : pendingFetch;
  }
  if (profiles.saving) return Promise.resolve();
  const requestGeneration = generation;
  profiles.loading = true;
  pendingFetch = (async () => {
    try {
      const response = await fetcher.get<{ profiles: unknown[] }>("/profiles");
      if (requestGeneration === generation) accept(decodeProfiles(response), !profilesDirty());
    } catch (error) {
      if (requestGeneration === generation) profiles.failed = true;
      console.error("Failed to fetch profiles", error);
    } finally {
      profiles.loading = false;
      pendingFetch = undefined;
    }
  })();
  return pendingFetch;
}
export function resetProfileDraft() {
  // Discard edits, not a failed disk write: the latter still needs a retry.
  accept(profiles.list, true);
}
export async function saveProfiles(values: readonly Profile[] = profiles.draft): Promise<void> {
  if (profiles.saving) throw new Error("Profile save already in progress");
  const clean = cleanProfiles(values);
  const error = profileError(clean);
  if (error) throw new Error(error);
  profiles.saving = true;
  generation++;
  try {
    const response = await fetcher.put<{ profiles: unknown[] }>("/profiles", { profiles: clean });
    accept(decodeProfiles(response), true);
    profiles.persistencePending = false;
  } catch (error) {
    if (error instanceof HttpError && error.status === 500) {
      const body = error.data as
        | { code?: string; applied?: boolean; profiles?: unknown[] }
        | undefined;
      if (
        body?.code === "PERSISTENCE_FAILED" &&
        body.applied === true &&
        Array.isArray(body.profiles)
      ) {
        try {
          accept(decodeProfiles({ profiles: body.profiles }), true);
          profiles.persistencePending = true;
        } catch {
          /* A malformed acknowledgement is not a new baseline. */
        }
      }
    }
    throw error;
  } finally {
    profiles.saving = false;
  }
}
