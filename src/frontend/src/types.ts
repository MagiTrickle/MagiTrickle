import {
  array,
  boolean,
  fallback,
  length,
  number,
  object,
  optional,
  parse,
  pipe,
  regex,
  string,
  type InferOutput,
} from "valibot";

import { randomId } from "./utils/defaults";
import {
  DEFAULT_GROUP_PRIORITY,
  DEFAULT_SUBSCRIPTION_PRIORITY,
  PrioritySchema,
} from "./utils/priority";

declare global {
  interface WindowEventMap {
    overlay: CustomEvent<{
      content: string;
      type: "show" | "hide";
    }>;

    toast: CustomEvent<{
      content: string;
      type: "info" | "success" | "error" | "warning";
    }>;
  }
}

export function parseConfig(json: string): Config {
  return parse(ConfigSchema, JSON.parse(json));
}

export const RuleSchema = object({
  enable: fallback(boolean(), true),
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  rule: string(),
  type: fallback(string(), "namespace"),
});
export type Rule = InferOutput<typeof RuleSchema>;

export const GroupSchema = object({
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  color: fallback(optional(string()), "#ffffff"),
  interface: string(),
  profile: optional(string()),
  enable: fallback(boolean(), true),
  priority: optional(PrioritySchema, DEFAULT_GROUP_PRIORITY),
  rules: array(RuleSchema),
});
export type Group = InferOutput<typeof GroupSchema>;

export const SubscriptionRuleSchema = object({
  enable: fallback(boolean(), true),
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  rule: string(),
  type: fallback(string(), "namespace"),
});
export type SubscriptionRule = InferOutput<typeof SubscriptionRuleSchema>;

export const SubscriptionSchema = object({
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  interface: string(),
  profile: optional(string()),
  enable: fallback(boolean(), true),
  priority: optional(PrioritySchema, DEFAULT_SUBSCRIPTION_PRIORITY),
  rules: array(SubscriptionRuleSchema),
  url: string(),
  lastUpdate: fallback(optional(number()), 0),
  interval: fallback(optional(number()), 86400),
});
export type Subscription = InferOutput<typeof SubscriptionSchema>;

export const ProfileSchema = object({
  id: string(),
  name: string(),
  interfaces: array(string()),
  on_unavailable: optional(string(), "blackhole"),
  usage: optional(object({ groups: number(), subscriptions: number() })),
});
export type Profile = InferOutput<typeof ProfileSchema>;

export const ConfigSchema = object({
  groups: array(GroupSchema),
  profiles: optional(array(ProfileSchema)),
});
export type Config = InferOutput<typeof ConfigSchema>;

export const RULE_TYPES = [
  { value: "namespace", label: "Namespace" },
  { value: "wildcard", label: "Wildcard" },
  { value: "regex", label: "Regex" },
  { value: "domain", label: "Domain" },
  { value: "subnet", label: "IPv4 subnet" },
  { value: "subnet6", label: "IPv6 subnet" },
];

export type Interfaces = {
  interfaces: {
    id: string;
    name?: string;
  }[];
};
