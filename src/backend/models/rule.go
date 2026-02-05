package models

import (
	"sync"

	"magitrickle/utils/intID"

	"github.com/IGLOU-EU/go-wildcard/v2"
	"github.com/dlclark/regexp2"
)

const (
	RuleTypeDomain    string = "domain"
	RuleTypeNamespace string = "namespace"
	RuleTypeWildcard  string = "wildcard"
	RuleTypeRegEx     string = "regex"
	RuleTypeSubnet    string = "subnet"
	RuleTypeSubnet6   string = "subnet6"
)

type Rule struct {
	ID     intID.ID
	Name   string
	Type   string
	Rule   string
	Enable bool

	compileOnce sync.Once
	compileWait sync.WaitGroup
	compileErr  error
	compiled    func(string) bool
}

func (d *Rule) IsEnabled() bool {
	return d.Enable
}

func (d *Rule) Compile() error {
	d.compileOnce.Do(func() {
		d.compileWait.Add(1)
		defer d.compileWait.Done()

		switch d.Type {
		case RuleTypeRegEx:
			re, err := regexp2.Compile(d.Rule, regexp2.IgnoreCase)
			if err != nil {
				d.compileErr = err
				d.compiled = func(string) bool { return false }
				return
			}
			d.compiled = func(s string) bool {
				ok, _ := re.MatchString(s)
				return ok
			}
		}
	})
	d.compileWait.Wait()
	return d.compileErr
}

func (d *Rule) IsMatch(domainName string) bool {
	switch d.Type {
	case RuleTypeDomain:
		return domainName == d.Rule

	case RuleTypeNamespace:
		if domainName == d.Rule {
			return true
		}

		ruleLen := len(d.Rule)
		domainLen := len(domainName)
		if domainLen < ruleLen+1 {
			return false
		}
		return domainName[domainLen-ruleLen-1] == '.' && domainName[domainLen-ruleLen:] == d.Rule

	case RuleTypeWildcard:
		return wildcard.Match(d.Rule, domainName)

	case RuleTypeRegEx:
		err := d.Compile()
		if err != nil {
			return false
		}
		return d.compiled(domainName)

	case RuleTypeSubnet, RuleTypeSubnet6:
		return false
	}
	return false
}
