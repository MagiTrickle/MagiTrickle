import { strictEqual } from "node:assert";
import {
  isValidDomain,
  isValidNamespace,
  isValidRegex,
  isValidWildcard,
} from "../src/utils/rule-validators.ts";

Deno.test("regexp", () => {
  strictEqual(isValidRegex("^[a-zA-Z0-9]+$"), true);
  strictEqual(isValidRegex("^[a-zA-Z0-9+$"), false);
  strictEqual(isValidRegex(".*"), true);
  strictEqual(isValidRegex("a(b|c)d"), true);
  strictEqual(isValidRegex("\\d+"), true);
});

Deno.test("domain", () => {
  strictEqual(isValidDomain("domain.com"), true);
  strictEqual(isValidDomain("domain.com.cn"), true);
  strictEqual(isValidDomain("domain.com.cn.cn"), true);
  strictEqual(isValidDomain(".com.cn.cn.cn"), false);
  strictEqual(isValidDomain("com.cn.cn."), false);
  strictEqual(isValidDomain("sub.domain.com"), true);
  strictEqual(isValidDomain("sub-domain.com"), true);
  strictEqual(isValidDomain("sub.domain-test.com"), true);
  strictEqual(isValidDomain("123.domain.com"), true);
  strictEqual(isValidDomain("domain.123.com"), true);
  strictEqual(isValidDomain("domain.com123"), true);
  strictEqual(isValidDomain("domain.com-"), false);
  strictEqual(isValidDomain("-domain.com"), false);
  strictEqual(isValidDomain("domain.-com"), false);
  strictEqual(isValidDomain("domain..com"), false);
  strictEqual(isValidDomain(".domain"), false);
  strictEqual(isValidDomain("domain.123"), true);
  strictEqual(isValidDomain("domain.123.123"), true);
  strictEqual(isValidDomain("a.b.c.d"), true);
  strictEqual(isValidDomain("a.b.c.123"), true);
  strictEqual(isValidDomain("a.b.c.d-"), false);
  strictEqual(isValidDomain("a.b.c.-d"), false);
  strictEqual(isValidDomain("a.b.c..d"), false);
  strictEqual(isValidDomain("a.b.c."), false);
  strictEqual(isValidDomain(".a.b.c"), false);
});

Deno.test("wildcard", () => {
  strictEqual(isValidWildcard("*.domain.com"), true);
  strictEqual(isValidWildcard("*.sub.domain.com"), true);
  strictEqual(isValidWildcard("domain.com"), true);
  strictEqual(isValidWildcard("sub.domain.com"), true);
  strictEqual(isValidWildcard("*.domain.com.cn"), true);
  strictEqual(isValidWildcard("*.sub.domain.com.cn"), true);
  strictEqual(isValidWildcard("domain.com.cn"), true);
  strictEqual(isValidWildcard("sub.domain.com.cn"), true);
  strictEqual(isValidWildcard("*.domain.com.cn.cn"), true);
  strictEqual(isValidWildcard("*.sub.domain.com.cn.cn"), true);
  strictEqual(isValidWildcard("domain.com.cn.cn"), true);
  strictEqual(isValidWildcard("sub.domain.com.cn.cn"), true);
  strictEqual(isValidWildcard("*.domain.123"), true);
  strictEqual(isValidWildcard("domain.123"), true);
  strictEqual(isValidWildcard("*.domain.123.123"), true);
  strictEqual(isValidWildcard("domain.123.123"), true);
  strictEqual(isValidWildcard("*.domain.com-"), false);
  strictEqual(isValidWildcard("*.domain.-com"), false);
  strictEqual(isValidWildcard("*.domain..com"), false);
  strictEqual(isValidWildcard("*.domain"), true);
  strictEqual(isValidWildcard("*.domain."), false);
  strictEqual(isValidWildcard(".*.domain"), false);
  strictEqual(isValidWildcard("*.domain."), false);
  strictEqual(isValidWildcard("*."), false);
  strictEqual(isValidWildcard("*."), false);
  strictEqual(isValidWildcard("*.domain"), true);
  strictEqual(isValidWildcard("domain"), true);
  strictEqual(isValidWildcard("*domain.com"), true);
});

Deno.test("namespace", () => {
  strictEqual(isValidNamespace("domain.com"), true);
  strictEqual(isValidNamespace("domain.com.cn"), true);
  strictEqual(isValidNamespace("domain.com.cn.cn"), true);
  strictEqual(isValidNamespace(".com.cn.cn.cn"), true); // Leading dot allowed
  strictEqual(isValidNamespace("com.cn.cn."), false);
  strictEqual(isValidNamespace("sub.domain.com"), true);
  strictEqual(isValidNamespace("sub-domain.com"), true);
  strictEqual(isValidNamespace("sub.domain-test.com"), true);
  strictEqual(isValidNamespace("123.domain.com"), true);
  strictEqual(isValidNamespace("domain.123.com"), true);
  strictEqual(isValidNamespace("domain.com123"), true);
  strictEqual(isValidNamespace("domain.com-"), false);
  strictEqual(isValidNamespace("-domain.com"), false);
  strictEqual(isValidNamespace("domain.-com"), false);
  strictEqual(isValidNamespace("domain..com"), false);
  strictEqual(isValidNamespace(".domain"), true); // Leading dot allowed
  strictEqual(isValidNamespace("domain.123"), true);
  strictEqual(isValidNamespace("domain.123.123"), true);
  strictEqual(isValidNamespace("a.b.c.d"), true);
  strictEqual(isValidNamespace("a.b.c.123"), true);
  strictEqual(isValidNamespace("a.b.c.d-"), false);
  strictEqual(isValidNamespace("a.b.c.-d"), false);
  strictEqual(isValidNamespace("a.b.c..d"), false);
  strictEqual(isValidNamespace("a.b.c."), false);
  strictEqual(isValidNamespace(".a.b.c"), true); // Leading dot allowed
  strictEqual(isValidNamespace("."), false);
  strictEqual(isValidNamespace(".."), false);
  strictEqual(isValidNamespace("..."), false);
  strictEqual(isValidNamespace("....domain.com"), true);
  strictEqual(isValidNamespace("domain.com...."), false); // trailing dots are not allowed
});
