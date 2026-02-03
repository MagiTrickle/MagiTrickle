package models

import (
	"testing"
)

func TestDomain_IsMatch_Domain(t *testing.T) {
	rule := &Rule{
		Type: RuleTypeDomain,
		Rule: "example.com",
	}
	if !rule.IsMatch("example.com") {
		t.Fatal("&Rule{Type: \"domain\", Rule: \"example.com\"}.IsMatch(\"example.com\") returns false")
	}
	if rule.IsMatch("noexample.com") {
		t.Fatal("&Rule{Type: \"domain\", Rule: \"example.com\"}.IsMatch(\"noexample.com\") returns true")
	}
}

func TestDomain_IsMatch_Wildcard(t *testing.T) {
	rule := &Rule{
		Type: RuleTypeWildcard,
		Rule: "ex*le.com",
	}
	if !rule.IsMatch("example.com") {
		t.Fatal("&Rule{Type: \"wildcard\", Rule: \"ex*le.com\"}.IsMatch(\"example.com\") returns false")
	}
	if rule.IsMatch("noexample.com") {
		t.Fatal("&Rule{Type: \"wildcard\", Rule: \"ex*le.com\"}.IsMatch(\"noexample.com\") returns true")
	}
}

func TestDomain_IsMatch_RegEx(t *testing.T) {
	rule := &Rule{
		Type: "regex",
		Rule: "^ex[apm]{3}le.com$",
	}
	if !rule.IsMatch("example.com") {
		t.Fatal("&Rule{Type: \"regex\", Rule: \"^ex[apm]{3}le.com$\"}.IsMatch(\"example.com\") returns false")
	}
	if rule.IsMatch("noexample.com") {
		t.Fatal("&Rule{Type: \"regex\", Rule: \"^ex[apm]{3}le.com$\"}.IsMatch(\"noexample.com\") returns true")
	}
}
