import { randomId } from "./utils/defaults.ts";
import {
  parse,
  boolean,
  fallback,
  pipe,
  string,
  object,
  check,
  array,
  regex,
  length,
  type InferOutput,
} from "valibot";

declare global {
  interface WindowEventMap {
    rule_drop: CustomEvent<{
      from_group_index: number;
      from_rule_index: number;
      to_group_index: number;
      to_rule_index: number;
    }>;

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
  enable: boolean(),
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  rule: string(),
  type: pipe(
    string(),
    check((value) => RULE_TYPES.map((type) => type.value).includes(value))
  ),
});
export type Rule = InferOutput<typeof RuleSchema>;

export const GroupSchema = object({
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  color: string(),
  interface: string(),
  enable: boolean(),
  rules: array(RuleSchema),
});
export type Group = InferOutput<typeof GroupSchema>;

export const ConfigSchema = object({
  groups: array(GroupSchema),
});
export type Config = InferOutput<typeof ConfigSchema>;

export const RULE_TYPES = [
  { value: "namespace", label: "Namespace" },
  { value: "wildcard", label: "Wildcard" },
  { value: "regex", label: "Regex" },
  { value: "domain", label: "Domain" },
  { value: "ipnet", label: "IP Net" },
];

export type Interfaces = {
  interfaces: {
    id: string;
  }[];
};
