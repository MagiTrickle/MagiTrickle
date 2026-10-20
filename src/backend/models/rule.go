package models

import (
	"sync"

	"magitrickle/utils/intID"

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
	ID     intID.ID `yaml:"id"`
	Name   string   `yaml:"name"`
	Type   string   `yaml:"type"`
	Rule   string   `yaml:"rule"`
	Enable bool     `yaml:"enable"`

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
		return matchWildcard(d.Rule, domainName)

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

// matchWildcard сопоставляет имя с шаблоном, где '*' — любая
// последовательность символов (в том числе пустая), '?' — ровно один
// символ, а все остальные символы, включая '.', сравниваются буквально.
func matchWildcard(pattern, name string) bool {
	p, n := 0, 0
	star, retry := -1, 0
	for n < len(name) {
		switch {
		case p < len(pattern) && pattern[p] == '*':
			star, retry = p, n
			p++
		case p < len(pattern) && (pattern[p] == '?' || pattern[p] == name[n]):
			p++
			n++
		case star >= 0:
			// Откатываемся к последней звёздочке и отдаём ей ещё один символ.
			retry++
			p, n = star+1, retry
		default:
			return false
		}
	}
	for p < len(pattern) && pattern[p] == '*' {
		p++
	}
	return p == len(pattern)
}
