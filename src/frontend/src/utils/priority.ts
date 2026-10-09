import { integer, maxValue, minValue, number, pipe } from "valibot";

export const MIN_PRIORITY = 1;
export const MAX_PRIORITY = 1000;
export const DEFAULT_GROUP_PRIORITY = 300;
export const DEFAULT_SUBSCRIPTION_PRIORITY = 100;

export const PrioritySchema = pipe(
  number(),
  integer(),
  minValue(MIN_PRIORITY),
  maxValue(MAX_PRIORITY),
);

export function isValidPriority(value: unknown): value is number {
  return (
    typeof value === "number" &&
    Number.isInteger(value) &&
    value >= MIN_PRIORITY &&
    value <= MAX_PRIORITY
  );
}

/** Older API responses omit priority. Preserve explicit values for validation. */
export function withPriorityDefault<T extends { priority?: number }>(
  item: T,
  defaultPriority: number,
): T & { priority: number } {
  return {
    ...item,
    priority: item.priority === undefined ? defaultPriority : item.priority,
  };
}
