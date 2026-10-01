import type { Subscription, SubscriptionRule } from "../../types";

export const snapshotRules = (rules: readonly SubscriptionRule[]): SubscriptionRule[] =>
  rules.map((rule) => ({ ...rule }));

export type RuleChange = {
  id: string;
  rule: string;
  previousType: string;
  previousEnable: boolean;
  type: string;
  enable: boolean;
};

/** The editor changes type/enable, not rule text/order. Keep all unchanged
 * rules on the server and send optimistic preconditions for actual edits. */
export function buildSubscriptionUpdate(
  subscription: Subscription,
  baseline: readonly SubscriptionRule[] | undefined,
) {
  const stale = () => new Error("Subscription rules changed; reload before saving");
  if (!baseline || baseline.length !== subscription.rules.length) throw stale();
  const byId = new Map(baseline.map((rule) => [rule.id, rule]));
  if (byId.size !== baseline.length) throw stale();
  const seen = new Set<string>();
  const ruleChanges: RuleChange[] = [];
  for (const rule of subscription.rules) {
    const previous = byId.get(rule.id);
    if (!previous || previous.rule !== rule.rule || seen.has(rule.id)) throw stale();
    seen.add(rule.id);
    if (previous.type !== rule.type || previous.enable !== rule.enable) {
      ruleChanges.push({
        id: rule.id,
        rule: rule.rule,
        previousType: previous.type,
        previousEnable: previous.enable,
        type: rule.type,
        enable: rule.enable,
      });
    }
  }
  // Do not send a stale lastUpdate timestamp: the server may have refreshed
  // untouched rules while the user was editing subscription metadata.
  return {
    id: subscription.id,
    name: subscription.name,
    url: subscription.url.trim(),
    interface: subscription.interface,
    enable: subscription.enable,
    interval: subscription.interval,
    ruleChanges,
  };
}
