import { fetcher } from "../../../utils/fetcher";
import { HttpError } from "../../../utils/http-error";

export type PolicySettings = { enabled: boolean; policy_id: string; mode: "include" | "exclude" };
export type Policy = { id: string; name: string; available: boolean };
export type Status = {
  settings: PolicySettings;
  runtime: { state: "disabled" | "ready" | "applying" | "unavailable" | "error"; reason?: string };
};
const defaults: PolicySettings = { enabled: false, policy_id: "", mode: "include" };
export const policy = $state({
  draft: { ...defaults },
  baseline: JSON.stringify(defaults),
  runtime: { state: "disabled" } as Status["runtime"],
  loaded: false,
  loading: false,
  failed: false,
  saving: false,
  persistencePending: false,
  policies: [] as Policy[],
  listing: false,
  listLoaded: false,
  listFailed: false,
});
export function selectedPolicyId() {
  return policy.draft.policy_id || policy.policies.find((p) => p.name === "mt")?.id || "";
}
export function policyDirty() {
  return policy.persistencePending || JSON.stringify(policy.draft) !== policy.baseline;
}
function decodeStatus(value: unknown): Status {
  const v = value as Status;
  if (
    !v?.settings ||
    typeof v.settings.enabled !== "boolean" ||
    typeof v.settings.policy_id !== "string" ||
    !["include", "exclude"].includes(v.settings.mode) ||
    !v.runtime ||
    !["disabled", "ready", "applying", "unavailable", "error"].includes(v.runtime.state)
  ) {
    throw new Error("Invalid policy settings response");
  }
  return {
    settings: {
      enabled: v.settings.enabled,
      policy_id: v.settings.policy_id,
      mode: v.settings.mode,
    },
    runtime: { state: v.runtime.state, reason: v.runtime.reason },
  };
}
function accept(value: Status, resetDraft: boolean) {
  policy.runtime = value.runtime;
  policy.loaded = true;
  policy.failed = false;
  if (resetDraft) {
    policy.draft = { ...value.settings };
    policy.baseline = JSON.stringify(value.settings);
  }
}
let pending: Promise<void> | undefined;
let generation = 0;
export function fetchPolicySettings(silent = false): Promise<void> {
  if (pending) return pending;
  if (policy.saving) return Promise.resolve();
  const version = generation;
  policy.loading = true;
  pending = (async () => {
    try {
      const value = decodeStatus(
        await fetcher.get<unknown>("/keenetic/policy", { silent, timeoutMs: 5000 }),
      );
      if (version === generation) accept(value, !policy.loaded || !policyDirty());
    } catch {
      if (version === generation && !policy.loaded) policy.failed = true;
    } finally {
      policy.loading = false;
      pending = undefined;
    }
  })();
  return pending;
}
export async function refreshPolicies() {
  if (policy.listing) return;
  policy.listing = true;
  try {
    const value = await fetcher.get<{ policies: Policy[] }>("/keenetic/policies", {
      timeoutMs: 5000,
    });
    if (
      !Array.isArray(value?.policies) ||
      value.policies.length > 256 ||
      value.policies.some(
        (p) =>
          typeof p.id !== "string" ||
          !p.id ||
          typeof p.name !== "string" ||
          typeof p.available !== "boolean",
      )
    ) {
      throw new Error("Invalid policy list response");
    }
    if (new Set(value.policies.map((p) => p.id)).size !== value.policies.length)
      throw new Error("Duplicate policy ID");
    policy.policies = value.policies;
    policy.listLoaded = true;
    policy.listFailed = false;
  } catch {
    policy.listFailed = true;
  } finally {
    policy.listing = false;
  }
}
export async function savePolicySettings() {
  if (policy.saving) return;
  policy.saving = true;
  generation++;
  try {
    accept(
      decodeStatus(
        await fetcher.put<unknown>("/keenetic/policy", {
          ...policy.draft,
          policy_id: policy.draft.enabled ? selectedPolicyId() : policy.draft.policy_id,
        }),
      ),
      true,
    );
    policy.persistencePending = false;
  } catch (error) {
    if (error instanceof HttpError) {
      const body = error.data as { code?: string; applied?: boolean } | undefined;
      if (body?.code === "PERSISTENCE_FAILED" && body.applied === true) {
        try {
          accept(decodeStatus(error.data), true);
          policy.persistencePending = true;
        } catch {
          /* A malformed acknowledgement cannot reset the draft. */
        }
      }
    }
    throw error;
  } finally {
    policy.saving = false;
  }
}
