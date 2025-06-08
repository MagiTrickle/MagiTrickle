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
		re, err := regexp2.Compile(d.Rule, regexp2.IgnoreCase)
		if err != nil {
			return false
		}
		ok, _ := re.MatchString(domainName)
		return ok
	case "domain":
		return domainName == d.Rule
	case "namespace":
		if domainName == d.Rule {
			return true
		}
		return strings.HasSuffix(domainName, "."+d.Rule)
	case "subnet":
		return false
	}
	return false
}
