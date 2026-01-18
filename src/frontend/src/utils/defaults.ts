import type { Group, Rule } from "../types";
import { interfaces } from "../data/interfaces.svelte";
import { randomDarkishColor } from "./colors";

export function defaultGroup(): Group {
  return {
    enable: true,
    id: randomId(),
    interface: interfaces.list.at(0) ?? "",
    name: "",
    color: randomDarkishColor(),
    rules: [],
  };
}

export function defaultRule(): Rule {
  return {
    enable: true,
    id: randomId(),
    name: "",
    rule: "",
    type: "namespace",
  };
}

export function randomId(length = 8) {
  const characters = "0123456789abcdef";
  let result = "";

  for (let i = 0; i < length; i++) {
    const randomIndex = Math.floor(Math.random() * characters.length);
    result += characters[randomIndex];
  }

  return result;
}
