import type { Group, Rule } from "../../types";

export function buildGroupUpdate(group: Group, baseline: readonly Rule[] | undefined) {
  const { rules, ...metadata } = group;
  if (
    !baseline ||
    baseline.length !== rules.length ||
    baseline.some((rule, index) => rule.id !== rules[index].id)
  ) {
    return group; // New/imported/moved/removed rules: preserve full ordered replacement.
  }
  const ruleChanges = [];
  for (let i = 0; i < rules.length; i++) {
    const before = baseline[i],
      after = rules[i];
    if (
      before.name !== after.name ||
      before.type !== after.type ||
      before.rule !== after.rule ||
      before.enable !== after.enable
    ) {
      ruleChanges.push({
        ...after,
        previous: {
          name: before.name,
          type: before.type,
          rule: before.rule,
          enable: before.enable,
        },
      });
    }
  }
  return { ...metadata, ruleChanges };
}

export function snapshotGroupRules(groups: readonly Group[]) {
  return new Map(groups.map((group) => [group.id, group.rules.map((rule) => ({ ...rule }))]));
}
