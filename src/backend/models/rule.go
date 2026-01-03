package models

import (
	"strings"

	"magitrickle/utils/intID"

	"github.com/IGLOU-EU/go-wildcard/v2"
	"github.com/dlclark/regexp2"
)

type Rule struct {
	ID     intID.ID
	Name   string
	Type   string
	Rule   string
	Enable bool

	compiled func(string) bool
}

func (d *Rule) IsEnabled() bool {
	return d.Enable
}

func (d *Rule) IsMatch(domainName string) bool {
	switch d.Type {
	case "wildcard":
		return wildcard.Match(d.Rule, domainName)
	case "regex":
		if d.compiled != nil {
			return d.compiled(domainName)
		}
		re, err := regexp2.Compile(d.Rule, regexp2.IgnoreCase)
		if err != nil {
			return false
		}
		compiled := func(s string) bool {
			ok, _ := re.MatchString(s)
			return ok
		}
		d.compiled = compiled
		return d.compiled(domainName)
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
