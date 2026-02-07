import { type Group, type Rule } from "../../types";
import { sortRules, type SortDirection, type SortField } from "../../utils/rule-sorter";

export type YieldToMain = () => Promise<void>;

function randomId(length = 8) {
  const characters = "0123456789abcdef";
  let result = "";
  for (let i = 0; i < length; i++) {
    const randomIndex = Math.floor(Math.random() * characters.length);
    result += characters[randomIndex];
  }
  return result;
}

export function cloneGroupWithNewIds(group: Group): Group {
  return {
    ...group,
    id: randomId(),
    rules: group.rules.map((rule) => ({
      ...rule,
      id: randomId(),
    })),
  };
}

export async function cloneGroupsWithNewIds(
  groups: Group[],
  yieldToMain?: YieldToMain,
  chunkSize = 20,
) {
  const cloned: Group[] = [];
  const now =
    typeof performance !== "undefined" && typeof performance.now === "function"
      ? () => performance.now()
      : () => Date.now();
  let lastYieldAt = now();
  const maybeYield = async (force = false) => {
    if (!yieldToMain) return;
    if (force || now() - lastYieldAt > 8) {
      await yieldToMain();
      lastYieldAt = now();
    }
  };

  for (let i = 0; i < groups.length; i += chunkSize) {
    const end = Math.min(i + chunkSize, groups.length);
    for (let index = i; index < end; index++) {
      const source = groups[index];
      const clonedRules: Rule[] = [];

      for (let ruleIndex = 0; ruleIndex < source.rules.length; ruleIndex++) {
        clonedRules.push({
          ...source.rules[ruleIndex],
          id: randomId(),
        });
        if (ruleIndex < source.rules.length - 1) {
          await maybeYield();
        }
      }

      cloned.push({
        ...source,
        id: randomId(),
        rules: clonedRules,
      });

      if (index < groups.length - 1) {
        await maybeYield();
      }
    }
    if (end < groups.length) {
      await maybeYield(true);
    }
  }

  return cloned;
}

export async function prependGroups(
  target: Group[],
  openState: Record<string, boolean>,
  groups: Group[],
  yieldToMain?: YieldToMain,
  chunkSize = 25,
) {
  for (let end = groups.length; end > 0; end -= chunkSize) {
    const start = Math.max(0, end - chunkSize);
    const chunk = groups.slice(start, end);

    target.unshift(...chunk);
    for (let i = 0; i < chunk.length; i++) {
      openState[chunk[i].id] = true;
    }

    if (yieldToMain && start > 0) {
      await yieldToMain();
    }
  }
}

export async function prependRules(
  group: Group,
  rules: Rule[],
  yieldToMain?: YieldToMain,
  chunkSize = 300,
) {
  for (let end = rules.length; end > 0; end -= chunkSize) {
    const start = Math.max(0, end - chunkSize);
    const chunk = rules.slice(start, end);

    group.rules.unshift(...chunk);

    if (yieldToMain && start > 0) {
      await yieldToMain();
    }
  }
}

export function sortGroupRules(group: Group, field: SortField, direction: SortDirection) {
  const sorted = sortRules(group.rules, field, direction);
  group.rules.splice(0, group.rules.length, ...sorted);
}

export function restoreGroupRulesOrder(group: Group, ruleIds: string[]) {
  const ruleMap = new Map(group.rules.map((rule) => [rule.id, rule]));
  const orderedRules = ruleIds
    .map((id) => ruleMap.get(id))
    .filter((rule): rule is Rule => Boolean(rule));

  if (!orderedRules.length) return false;

  group.rules.splice(0, group.rules.length, ...orderedRules);
  return true;
}

export function toConfigPayload(groups: Group[]) {
  return { groups: structuredClone(groups) };
}
