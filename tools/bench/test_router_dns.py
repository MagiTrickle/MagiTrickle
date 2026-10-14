"""Ensure private production settings never enter the benchmark config."""
import tempfile
import unittest
from pathlib import Path

import yaml

from router_dns import live_rules_config


class LiveRulesIsolationTest(unittest.TestCase):
    def test_private_snapshot_detaches_routing_and_fetch(self):
        source = dict(app={"password": "fixture-secret", "link": ["production-br"]},
                      profiles=[{"id": "production-profile"}],
                      groups=[dict(id="11111111", name="private-name", enable=True,
                                   interface="production-wan", profile="production-profile",
                                   rules=[dict(id="33333333", type="namespace", rule="example.com", enable=True)]),
                              dict(id="22222222", enable=False, rules=[])],
                      subscriptions=[dict(id="44444444", name="private-sub", enable=True,
                                          interface="production-wan", url="https://example.com/private-token",
                                          interval=60, rules=[dict(id="55555555", type="subnet", rule="192.0.2.0/24", enable=True)])])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private.yaml"
            path.write_text(yaml.safe_dump(source))
            text, shape = live_rules_config(path, ["example.com", "github.com"], 53)
        target = yaml.safe_load(text)
        self.assertNotIn("fixture-secret", text)
        self.assertNotIn("private-token", text)
        self.assertNotIn("production-", text)
        self.assertNotIn("private-name", text)
        self.assertNotIn("profiles", target)
        self.assertTrue(target["app"]["netfilter"]["disableIPv4"])
        self.assertTrue(target["app"]["netfilter"]["disableIPv6"])
        self.assertTrue(target["app"]["dnsProxy"]["disableRemap53"])
        self.assertEqual(target["app"]["link"], [])
        self.assertEqual(len(target["groups"]), 1)
        self.assertEqual(target["subscriptions"][0]["url"], "")
        self.assertEqual(target["subscriptions"][0]["interval"], 0)
        entries = target["groups"] + target["subscriptions"]
        self.assertTrue(all(g["interface"] == "blackhole" and g["enable"] for g in entries))
        rule_ids = [r["id"] for g in entries for r in g["rules"]]
        self.assertEqual(len(rule_ids), len(set(rule_ids)))
        self.assertEqual(shape["total_rules"], 2)
        self.assertEqual(shape["rule_types"], {"namespace": 1, "subnet": 1})


if __name__ == "__main__":
    unittest.main()
