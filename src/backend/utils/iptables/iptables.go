package iptables

import (
	"bytes"
	"errors"
	"fmt"
	"slices"
	"strconv"
	"strings"
	"sync"
)

type Protocol byte

const (
	ProtocolIPv4 Protocol = 0
	ProtocolIPv6 Protocol = 1
)

var (
	ErrChainNotInitialized = errors.New("chain not initialized")
)

type IPTables struct {
	rules      map[string]map[string]chain
	executable Executable
	sync       sync.RWMutex
}

func NewIPTables(executable Executable) *IPTables {
	return &IPTables{
		rules:      make(map[string]map[string]chain),
		executable: executable,
	}
}

func (ipt *IPTables) Proto() Protocol {
	return ipt.executable.Proto()
}

func (ipt *IPTables) RegisterChainDelete(table, chainName string) error {
	ipt.sync.Lock()
	defer ipt.sync.Unlock()

	if _, ok := ipt.rules[table]; !ok {
		ipt.rules[table] = make(map[string]chain)
	}
	ipt.rules[table][chainName] = &chainDelete{}
	return nil
}

func (ipt *IPTables) RegisterChainOverlay(table, chainName string) error {
	ipt.sync.Lock()
	defer ipt.sync.Unlock()

	if _, ok := ipt.rules[table]; !ok {
		ipt.rules[table] = make(map[string]chain)
	}
	ipt.rules[table][chainName] = &chainOverlay{}
	return nil
}

func (ipt *IPTables) RegisterChainOverride(table, chainName string) error {
	ipt.sync.Lock()
	defer ipt.sync.Unlock()

	if _, ok := ipt.rules[table]; !ok {
		ipt.rules[table] = make(map[string]chain)
	}
	ipt.rules[table][chainName] = &chainOverride{}
	return nil
}

func (ipt *IPTables) Append(table, chain string, rule ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Append(rule)
}

func (ipt *IPTables) Insert(table, chain string, ruleNum int, rule ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Insert(ruleNum, rule)
}

func (ipt *IPTables) Delete(table, chain string, rule ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Delete(rule)
}

func (ipt *IPTables) GetCurrentRules() (map[string]map[string][]rule, error) {
	rules := make(map[string]map[string][]rule)

	data, err := ipt.executable.Save()
	if err != nil {
		return nil, err
	}

	lines := bytes.Split(data, []byte("\n"))
	currentTable := ""
	for _, line := range lines {
		if len(line) == 0 {
			continue
		}

		switch line[0] {
		case '*':
			currentTable = string(line[1:])
			if rules[currentTable] == nil {
				rules[currentTable] = make(map[string][]rule)
			}

		case ':':
			parts := strings.Fields(string(line[1:]))
			if len(parts) == 0 {
				return nil, fmt.Errorf("invalid chain declaration")
			}
			chain := parts[0]
			if rules[currentTable][chain] == nil {
				rules[currentTable][chain] = []rule{}
			}

		case '-':
			if currentTable == "" {
				return nil, fmt.Errorf("rule outside of table")
			}

			fields := strings.Fields(string(line))
			if len(fields) < 2 {
				return nil, fmt.Errorf("invalid rule: %q", line)
			}

			chain := fields[1]
			switch fields[0][1] {
			case 'A': // Append
				rules[currentTable][chain] = append(rules[currentTable][chain], fields[2:])

			default:
				return nil, fmt.Errorf("unknown iptables command %q", fields[0])
			}

		case '#':
			continue

		case 'C':
			currentTable = ""

		default:
			return nil, fmt.Errorf("unknown iptables input %q", line)
		}
	}

	return rules, nil
}

func (ipt *IPTables) Commit() error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	curRules, err := ipt.GetCurrentRules()
	if err != nil {
		return err
	}

	buf := new(bytes.Buffer)
	for tableName, table := range ipt.rules {
		curTable := curRules[tableName]

		buf.WriteString("*" + tableName + "\n")

		commandsPriorizied := make(map[int8][]command)
		for chainName, chain := range table {
			var curChain []rule
			if curTable[chainName] != nil {
				curChain = curTable[chainName]
			}
			commands, priority, err := chain.Compile(chainName, curChain)
			if err != nil {
				return err
			}
			if commandsPriorizied[int8(priority)] == nil {
				commandsPriorizied[int8(priority)] = make([]command, 0)
			}
			if len(commands) > 0 {
				buf.WriteString(":" + chainName + " - [0:0]\n")
				commandsPriorizied[int8(priority)] = append(commandsPriorizied[int8(priority)], commands...)
			}
		}

		priorities := make([]int8, 0, len(commandsPriorizied))
		for priority, _ := range commandsPriorizied {
			priorities = append(priorities, priority)
		}
		slices.Sort(priorities)

		for _, priority := range priorities {
			for _, command := range commandsPriorizied[priority] {
				var rule []string
				switch command.Option {
				case optionAppend:
					rule = make([]string, 2, len(command.Rule)+2)
					rule[0] = "-A"
					rule[1] = command.Chain
					rule = append(rule, command.Rule...)
				case optionDelete:
					rule = make([]string, 2, len(command.Rule)+2)
					rule[0] = "-D"
					rule[1] = command.Chain
					rule = append(rule, command.Rule...)
				case optionInsert:
					rule = make([]string, 3, len(command.Rule)+3)
					rule[0] = "-I"
					rule[1] = command.Chain
					rule[2] = strconv.Itoa(command.RuleNum)
					rule = append(rule, command.Rule...)
				case optionFlush:
					rule = []string{"-F", command.Chain}
				case optionDeleteChain:
					rule = []string{"-X", command.Chain}
				}
				if rule != nil {
					buf.WriteString(strings.Join(rule, " "))
					buf.WriteByte('\n')
				}
			}
		}
		buf.WriteString("COMMIT\n")
	}

	return ipt.executable.Restore(buf.Bytes())
}
