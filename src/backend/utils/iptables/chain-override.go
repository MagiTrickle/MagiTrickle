package iptables

import (
	"sync"
)

// Chain where the rules should override the existing ones
type chainOverride struct {
	rules []Rule
	sync  sync.RWMutex
}

func (c *chainOverride) Compile(chainName []byte, existedRules []Rule) ([]command, priority, error) {
	c.sync.RLock()
	defer c.sync.RUnlock()

	if existedRules != nil && len(existedRules) == len(c.rules) {
		match := true
		for idx, r := range c.rules {
			if !ruleEqual(r, existedRules[idx]) {
				match = false
				break
			}
		}
		if match {
			return nil, 0, nil
		}
	}

	out := make([]command, len(c.rules)+1)
	out[0] = command{Option: optionFlush, Chain: chainName}
	for i, r := range c.rules {
		out[i+1] = command{Option: optionAppend, Chain: chainName, Rule: r}
	}
	return out, -128, nil
}

func (c *chainOverride) Append(r Rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	c.rules = append(c.rules, r)
	return nil
}

func (c *chainOverride) Insert(ruleNum int, r Rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	if ruleNum < 1 || ruleNum > len(c.rules)+1 {
		return nil
	}
	insertIdx := ruleNum - 1
	c.rules = append(c.rules, nil)
	copy(c.rules[insertIdx+1:], c.rules[insertIdx:len(c.rules)-1])
	c.rules[insertIdx] = r
	return nil
}

func (c *chainOverride) Delete(r Rule) error {
	c.sync.Lock()
	defer c.sync.Unlock()

	for idx, existing := range c.rules {
		if !ruleEqual(existing, r) {
			continue
		}

		c.rules = append(c.rules[:idx], c.rules[idx+1:]...)
		return nil
	}
	return nil
}
