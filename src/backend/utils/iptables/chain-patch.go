package iptables

import (
	"sync"
)

// Chain where the rules should merge with the existing ones
// For the same Rule, only the last operation is kept (last-write-wins).
// Insert behaves as InsertUnique, not iptables -I.
type chainPatch struct {
	orderedRules []chainPatchRule
	sync         sync.RWMutex
}

type chainPatchRule struct {
	Option  option
	RuleNum int
	Rule    Rule
}

func (c *chainPatch) addRule(r chainPatchRule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	shiftedIdx := 0
	for idx := 0; idx < len(c.orderedRules); idx++ {
		if ruleEqual(c.orderedRules[idx].Rule, r.Rule) {
			continue
		}
		if shiftedIdx != idx {
			c.orderedRules[shiftedIdx] = c.orderedRules[idx]
		}
		shiftedIdx++
	}

	c.orderedRules = append(c.orderedRules[:shiftedIdx], r)
	return nil
}

func (c *chainPatch) Compile(chainName []byte, existedRules []Rule) ([]command, priority, error) {
	c.sync.RLock()
	defer c.sync.RUnlock()

	existedRulesMap := make(map[string]uint8)

	for _, r := range existedRules {
		key := r.String()
		existedRulesMap[key]++
	}

	var out []command
	for _, r := range c.orderedRules {
		key := r.Rule.String()
		count := existedRulesMap[key]

		// Removing duplicates
		for count > 1 {
			out = append(out, command{Option: optionDelete, Chain: chainName, Rule: r.Rule})
			count--
		}

		switch r.Option {
		case optionAppend:
			if count > 0 {
				continue
			}
			out = append(out, command{Option: optionAppend, Chain: chainName, Rule: r.Rule})
		case optionInsert:
			if count > 0 {
				continue
			}
			out = append(out, command{Option: optionInsert, RuleNum: r.RuleNum, Chain: chainName, Rule: r.Rule})
		case optionDelete:
			if count == 0 {
				continue
			}
			out = append(out, command{Option: optionDelete, Chain: chainName, Rule: r.Rule})
		}
	}
	return out, 0, nil
}

func (c *chainPatch) Append(rule Rule) error {
	return c.addRule(chainPatchRule{
		Rule:   rule,
		Option: optionAppend,
	})
}
func (c *chainPatch) Insert(ruleNum int, rule Rule) error {
	return c.addRule(chainPatchRule{
		Rule:    rule,
		Option:  optionInsert,
		RuleNum: ruleNum,
	})
}
func (c *chainPatch) Delete(rule Rule) error {
	return c.addRule(chainPatchRule{
		Rule:   rule,
		Option: optionDelete,
	})
}
