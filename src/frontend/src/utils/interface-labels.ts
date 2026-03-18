import type { InterfaceInfo } from "../types";

export function toInterfaceOption({ id, name }: InterfaceInfo) {
  return {
    value: id,
    label: id,
    description: name && name !== id ? name : undefined,
  };
}
