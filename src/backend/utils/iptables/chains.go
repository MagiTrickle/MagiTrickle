package iptables

import (
	"slices"
	"strings"
	"sync"
)

type priority int8

type chain interface {
	Compile(chainName string, existedRules []rule) ([]command, priority, error)
	Append(rule rule) error
	Insert(ruleNum int, rule rule) error
	Delete(rule rule) error
}

// Chain for removal
type chainDelete struct {
}

func (c *chainDelete) Compile(chainName string, existedRules []rule) ([]command, priority, error) {
	return []command{
		{Option: optionFlush, Chain: chainName},
		{Option: optionDeleteChain, Chain: chainName},
	}, 127, nil
}

// No rules operations for removed chain
func (c *chainDelete) Append(rule rule) error {
	return nil
}
func (c *chainDelete) Insert(ruleNum int, rule rule) error {
	return nil
}
func (c *chainDelete) Delete(rule rule) error {
	return nil
}

// Chain where the rules should merge with the existing ones
// For the same rule, only the last operation is kept (last-write-wins).
// Insert behaves as InsertUnique, not iptables -I.
type chainOverlay struct {
	orderedRules []chainOverlayRule
	sync         sync.RWMutex
}

type chainOverlayRule struct {
	Option  option
	RuleNum int
	Rule    rule
}

func (c *chainOverlay) addRule(rule chainOverlayRule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	shiftedIdx := 0
	for idx := 0; idx < len(c.orderedRules); idx++ {
		if slices.Equal(c.orderedRules[idx].Rule, rule.Rule) {
			continue
		}
		if shiftedIdx != idx {
			c.orderedRules[shiftedIdx] = c.orderedRules[idx]
		}
		shiftedIdx++
	}

	c.orderedRules = append(c.orderedRules[:shiftedIdx], rule)
	return nil
}

func (c *chainOverlay) Compile(chainName string, existedRules []rule) ([]command, priority, error) {
	c.sync.RLock()
	defer c.sync.RUnlock()

	existedRulesStr := make(map[string]struct{})

	for _, rule := range existedRules {
		key := strings.Join(rule, " ")
		existedRulesStr[key] = struct{}{}
	}

	var out []command
	for _, rule := range c.orderedRules {
		key := strings.Join(rule.Rule, " ")
		switch rule.Option {
		case optionAppend:
			if _, ok := existedRulesStr[key]; ok {
				continue
			}
			out = append(out, command{Option: optionAppend, Chain: chainName, Rule: rule.Rule})
		case optionInsert:
			if _, ok := existedRulesStr[key]; ok {
				continue
			}
			out = append(out, command{Option: optionInsert, RuleNum: rule.RuleNum, Chain: chainName, Rule: rule.Rule})
		case optionDelete:
			if _, ok := existedRulesStr[key]; !ok {
				continue
			}
			out = append(out, command{Option: optionDelete, Chain: chainName, Rule: rule.Rule})
		}
	}
	return out, 0, nil
}

func (c *chainOverlay) Append(rule rule) error {
	return c.addRule(chainOverlayRule{
		Rule:   rule,
		Option: optionAppend,
	})
}
func (c *chainOverlay) Insert(ruleNum int, rule rule) error {
	return c.addRule(chainOverlayRule{
		Rule:    rule,
		Option:  optionInsert,
		RuleNum: ruleNum,
	})
}
func (c *chainOverlay) Delete(rule rule) error {
	return c.addRule(chainOverlayRule{
		Rule:   rule,
		Option: optionDelete,
	})
}

// Chain where the rules should override the existing ones
type chainOverride struct {
	rules []rule
	sync  sync.RWMutex
}

func (c *chainOverride) Compile(chainName string, existedRules []rule) ([]command, priority, error) {
	c.sync.RLock()
	defer c.sync.RUnlock()

	out := make([]command, len(c.rules)+1)
	out[0] = command{Option: optionFlush, Chain: chainName}
	for i, rule := range c.rules {
		out[i+1] = command{Option: optionAppend, Chain: chainName, Rule: rule}
	}
	return out, -128, nil
}

func (c *chainOverride) Append(rule rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	c.rules = append(c.rules, rule)
	return nil
}

func (c *chainOverride) Insert(ruleNum int, rule rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	if ruleNum < 1 || ruleNum > len(c.rules)+1 {
		return nil
	}
	insertIdx := ruleNum - 1
	c.rules = append(c.rules, nil)
	copy(c.rules[insertIdx+1:], c.rules[insertIdx:len(c.rules)-1])
	c.rules[insertIdx] = rule
	return nil
}

func (c *chainOverride) Delete(rule rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	for idx, r := range c.rules {
		if slices.Equal(r, rule) {
			if idx == len(c.rules)-1 {
				c.rules = c.rules[:idx]
			} else {
				c.rules = append(c.rules[:idx], c.rules[idx+1:]...)
			}
			return nil
		}
	}
	return nil
}
