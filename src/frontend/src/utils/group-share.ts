import { INTERFACES } from "../data/interfaces.svelte";
import { RULE_TYPES, type Group, type Rule } from "../types";
import { randomId } from "./defaults";

const RULE_TYPE_VALUES = RULE_TYPES.map((item) => item.value);
const RULE_TYPE_INDEX = new Map(RULE_TYPE_VALUES.map((value, index) => [value, index]));

const textEncoder = typeof TextEncoder !== "undefined" ? new TextEncoder() : null;
const textDecoder = typeof TextDecoder !== "undefined" ? new TextDecoder() : null;
const BufferRef =
  typeof (globalThis as { Buffer?: unknown }).Buffer !== "undefined"
    ? (globalThis as { Buffer: any }).Buffer
    : null;

function utf8Encode(value: string): Uint8Array {
  if (textEncoder) return textEncoder.encode(value);
  const utf8 = unescape(encodeURIComponent(value));
  const bytes = new Uint8Array(utf8.length);
  for (let i = 0; i < utf8.length; i++) bytes[i] = utf8.charCodeAt(i);
  return bytes;
}

function utf8Decode(bytes: Uint8Array): string {
  if (textDecoder) return textDecoder.decode(bytes);
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return decodeURIComponent(escape(binary));
}

function writeVarint(value: number, out: number[]) {
  let v = Math.max(0, Math.floor(value)) >>> 0;
  while (v >= 0x80) {
    out.push((v & 0x7f) | 0x80);
    v >>>= 7;
  }
  out.push(v);
}

function readVarint(bytes: Uint8Array, offset: number): { value: number; next: number } {
  let result = 0;
  let shift = 0;
  let index = offset;
  while (index < bytes.length) {
    const byte = bytes[index++];
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return { value: result, next: index };
    shift += 7;
    if (shift > 35) break;
  }
  throw new Error("Invalid varint");
}

function writeString(value: string, out: number[]) {
  const encoded = utf8Encode(value);
  writeVarint(encoded.length, out);
  for (const byte of encoded) out.push(byte);
}

function readString(bytes: Uint8Array, offset: number): { value: string; next: number } {
  const { value: length, next } = readVarint(bytes, offset);
  const end = next + length;
  if (end > bytes.length) throw new Error("Invalid string length");
  const value = utf8Decode(bytes.subarray(next, end));
  return { value, next: end };
}

function base64UrlEncode(bytes: Uint8Array): string {
  if (typeof btoa === "function") {
    let binary = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
    }
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
  }
  if (BufferRef) {
    return BufferRef.from(bytes)
      .toString("base64")
      .replace(/\+/g, "-")
      .replace(/\//g, "_")
      .replace(/=+$/g, "");
  }
  throw new Error("Base64 encoding not available");
}

function base64UrlDecode(value: string): Uint8Array {
  const normalized = value.replace(/-/g, "+").replace(/_/g, "/");
  const padded = normalized + "===".slice((normalized.length + 3) % 4);
  if (typeof atob === "function") {
    const binary = atob(padded);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }
  if (BufferRef) {
    return new Uint8Array(BufferRef.from(padded, "base64"));
  }
  throw new Error("Base64 decoding not available");
}

function packGroup(group: Group): Uint8Array {
  const out: number[] = [];
  const name = typeof group.name === "string" ? group.name : "";
  const color = typeof group.color === "string" ? group.color : "#ffffff";
  const iface = typeof group.interface === "string" ? group.interface : "";
  const enable = group.enable !== false;
  let groupFlags = enable ? 1 : 0;
  if (name) groupFlags |= 2;

  out.push(groupFlags);
  if (name) writeString(name, out);
  writeString(color, out);
  writeString(iface, out);
  writeVarint(group.rules.length, out);

  for (const rule of group.rules) {
    const typeIndex = RULE_TYPE_INDEX.get(rule.type) ?? 0;
    const ruleName = typeof rule.name === "string" ? rule.name : "";
    const ruleEnable = rule.enable !== false;
    let ruleFlags = typeIndex & 0x0f;
    if (ruleEnable) ruleFlags |= 0x80;
    if (ruleName) ruleFlags |= 0x40;
    out.push(ruleFlags);
    if (ruleName) writeString(ruleName, out);
    writeString(typeof rule.rule === "string" ? rule.rule : "", out);
  }

  return new Uint8Array(out);
}

function unpackGroup(bytes: Uint8Array): Group {
  let offset = 0;
  if (bytes.length < 1) throw new Error("Invalid payload");
  const groupFlags = bytes[offset++];
  const enable = (groupFlags & 1) !== 0;
  const hasName = (groupFlags & 2) !== 0;

  let name = "";
  if (hasName) {
    const read = readString(bytes, offset);
    name = read.value;
    offset = read.next;
  }

  const colorRead = readString(bytes, offset);
  const color = colorRead.value;
  offset = colorRead.next;

  const ifaceRead = readString(bytes, offset);
  const ifaceRaw = ifaceRead.value;
  offset = ifaceRead.next;

  const rulesCountRead = readVarint(bytes, offset);
  const rulesCount = rulesCountRead.value;
  offset = rulesCountRead.next;
  if (rulesCount < 0) throw new Error("Invalid rules count");

  const rules: Rule[] = [];
  for (let i = 0; i < rulesCount; i++) {
    if (offset >= bytes.length) throw new Error("Unexpected end of payload");
    const ruleFlags = bytes[offset++];
    const ruleEnable = (ruleFlags & 0x80) !== 0;
    const hasRuleName = (ruleFlags & 0x40) !== 0;
    const typeIndex = ruleFlags & 0x0f;

    let ruleName = "";
    if (hasRuleName) {
      const nameRead = readString(bytes, offset);
      ruleName = nameRead.value;
      offset = nameRead.next;
    }

    const patternRead = readString(bytes, offset);
    const pattern = patternRead.value;
    offset = patternRead.next;

    const type = RULE_TYPE_VALUES[typeIndex] ?? RULE_TYPE_VALUES[0] ?? "namespace";
    rules.push({
      id: randomId(),
      name: ruleName,
      rule: pattern,
      type,
      enable: ruleEnable,
    });
  }

  const iface = ifaceRaw || (INTERFACES.at(0) ?? "");
  return {
    id: randomId(),
    name,
    color,
    interface: iface,
    enable,
    rules,
  };
}

export function encodeGroupShare(group: Group): string {
  return base64UrlEncode(packGroup(group));
}

export function decodeGroupShare(value: string): Group {
  if (typeof value !== "string") throw new Error("Invalid share string");
  const trimmed = value.trim().replace(/\s+/g, "");
  if (!trimmed) throw new Error("Invalid share string");
  return unpackGroup(base64UrlDecode(trimmed));
}
