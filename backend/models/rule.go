package models

import (
	"strings"

	"magitrickle/api/types"

	"github.com/IGLOU-EU/go-wildcard/v2"
	"github.com/dlclark/regexp2"
)

type Rule struct {
	ID     types.ID
	Name   string
	Type   string
	Rule   string
	Enable bool
}

func (d *Rule) IsEnabled() bool {
	return d.Enable
}

func (d *Rule) IsMatch(domainName string) bool {
	switch d.Type {
	case "wildcard":
		return wildcard.Match(d.Rule, domainName)
	case "regex":
		re := regexp2.MustCompile(d.Rule, 0)
		ok, _ := re.MatchString(domainName)
		return ok
	case "domain":
		return domainName == d.Rule
	case "namespace":
		if domainName == d.Rule {
			return true
		}
		return strings.HasSuffix(domainName, "."+d.Rule)
	}
	return false
}
