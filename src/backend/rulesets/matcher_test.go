package rulesets

import (
	"testing"

	"magitrickle/models"
)

// TestMatcherAgreesWithRule сверяет индекс с построчной проверкой
// models.Rule.IsMatch, которой он заменяет.
func TestMatcherAgreesWithRule(t *testing.T) {
	rules := []struct{ ruleType, rule string }{
		{models.RuleTypeDomain, "exact.com"},
		{models.RuleTypeNamespace, "example.com"},
		{models.RuleTypeNamespace, "com.ua"},
		{models.RuleTypeWildcard, "*.cdn.net"},
		{models.RuleTypeWildcard, "*video.org"},
		{models.RuleTypeWildcard, "api?.service.io"},
		{models.RuleTypeRegEx, `^ads\d+\.tracker\.com$`},
	}
	names := []string{
		"exact.com", "www.exact.com", "exact.com.evil",
		"example.com", "a.b.example.com", "notexample.com", "example.com.ru",
		"com.ua", "shop.com.ua", "ua",
		"a.cdn.net", "x.y.cdn.net", "cdn.net",
		"video.org", "myvideo.org", "a.video.org", "video.org.uk", "videoxorg",
		"xcdnxnet", "evil-cdn.net",
		"api1.service.io", "api.service.io",
		"ads42.tracker.com", "ads.tracker.com",
		"", "unrelated.net",
	}

	for _, r := range rules {
		var m Matcher
		m.add(r.ruleType, r.rule)
		rule := &models.Rule{Type: r.ruleType, Rule: r.rule, Enable: true}
		for _, name := range names {
			if got, want := m.Match(name), rule.IsMatch(name); got != want {
				t.Errorf("%s %q on %q: Match = %v, IsMatch = %v", r.ruleType, r.rule, name, got, want)
			}
		}
	}
}

func TestMatcherIndexesSimpleWildcards(t *testing.T) {
	var m Matcher
	m.add(models.RuleTypeWildcard, "*.cdn.net")
	m.add(models.RuleTypeWildcard, "*")
	m.add(models.RuleTypeWildcard, "api?.service.io")
	m.add(models.RuleTypeWildcard, "*.a*.io")

	if len(m.suffixes) != 2 || len(m.patterns) != 2 {
		t.Fatalf("suffixes = %v, patterns = %d; want 2 and 2", m.suffixes, len(m.patterns))
	}
	if !m.Match("anything") {
		t.Error(`"*" does not match everything`)
	}
}
