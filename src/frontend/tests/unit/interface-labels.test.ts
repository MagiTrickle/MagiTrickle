import assert from "node:assert";
import { describe, it } from "node:test";

import { toInterfaceOption } from "../../src/utils/interface-labels";

describe("Interface labels", () => {
  it("keeps interface id as the primary label", () => {
    assert.deepStrictEqual(toInterfaceOption({ id: "longinterf" }), {
      value: "longinterf",
      label: "longinterf",
      description: undefined,
    });
  });

  it("moves friendly label into secondary description", () => {
    assert.deepStrictEqual(toInterfaceOption({ id: "nwg0", name: "WireGuard tunnel" }), {
      value: "nwg0",
      label: "nwg0",
      description: "WireGuard tunnel",
    });
  });
});
