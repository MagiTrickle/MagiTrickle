import type { Group, Rule, Subscription, SubscriptionRule } from "../types";
import { HttpError } from "./http-error";

type RecordValue = Record<string, unknown>;
const isObject = (value: unknown): value is RecordValue =>
  value !== null && typeof value === "object" && !Array.isArray(value);
const isId = (value: unknown): value is string =>
  typeof value === "string" && /^[0-9a-f]{8}$/i.test(value);
const isSubRule = (value: unknown): value is SubscriptionRule =>
  isObject(value) &&
  isId(value.id) &&
  typeof value.rule === "string" &&
  typeof value.type === "string" &&
  typeof value.enable === "boolean";
const isRule = (value: unknown): value is Rule =>
  isSubRule(value) && typeof (value as unknown as RecordValue).name === "string";

function collection<T extends { id: string }>(
  value: unknown,
  valid: (item: unknown) => item is T,
): value is T[] {
  if (!Array.isArray(value)) return false;
  const ids = new Set<string>();
  return value.every((item) => {
    if (!valid(item) || ids.has(item.id)) return false;
    ids.add(item.id);
    return true;
  });
}

const isGroup = (value: unknown): value is Group =>
  isObject(value) &&
  isId(value.id) &&
  typeof value.name === "string" &&
  typeof value.color === "string" &&
  typeof value.interface === "string" &&
  typeof value.enable === "boolean" &&
  collection(value.rules, isRule);
const isSubscription = (value: unknown): value is Subscription =>
  isObject(value) &&
  isId(value.id) &&
  typeof value.name === "string" &&
  typeof value.interface === "string" &&
  typeof value.enable === "boolean" &&
  typeof value.url === "string" &&
  typeof value.interval === "number" &&
  Number.isFinite(value.interval) &&
  typeof value.lastUpdate === "number" &&
  Number.isFinite(value.lastUpdate) &&
  collection(value.rules, isSubRule);

/** Only a complete, explicit post-apply persistence failure can move the
 * optimistic baseline. Never guess that generic 500/409/network errors applied.
 * Do not use import schemas with fallback IDs here: IDs must be canonical. */
function appliedData(error: unknown): RecordValue | undefined {
  if (!(error instanceof HttpError) || error.status !== 500) return;
  const data = error.data;
  if (isObject(data) && data.code === "PERSISTENCE_FAILED" && data.applied === true) return data;
}

export function appliedGroups(error: unknown): Group[] | undefined {
  const groups = appliedData(error)?.groups;
  return collection(groups, isGroup) ? groups : undefined;
}

export function appliedSubscriptions(error: unknown): Subscription[] | undefined {
  const subscriptions = appliedData(error)?.subscriptions;
  return collection(subscriptions, isSubscription) ? subscriptions : undefined;
}
