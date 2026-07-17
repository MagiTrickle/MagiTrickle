//go:build testing

package iptables

import (
	"bytes"
	"fmt"
	"slices"
	"sort"
	"strconv"
	"strings"
)

type FakeIPTables struct {
	rules map[string]map[string][]Rule
	proto Protocol
}

func NewFakeIPTables(proto Protocol) *FakeIPTables {
	return &FakeIPTables{
		rules: make(map[string]map[string][]Rule),
		proto: proto,
	}
}

// SetInitialRules устанавливает начальное состояние таблиц для тестов
func (ipt *FakeIPTables) SetInitialRules(table, chain string, rules [][]string) {
	if ipt.rules[table] == nil {
		ipt.rules[table] = make(map[string][]Rule)
	}
	converted := make([]Rule, len(rules))
	for i, r := range rules {
		converted[i] = ruleFromStrings(r)
	}
	ipt.rules[table][chain] = converted
}

// GetRules возвращает текущие правила для тестовой проверки
func (ipt *FakeIPTables) GetRules(table, chain string) [][]string {
	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return nil
	}
	result := make([][]string, len(ipt.rules[table][chain]))
	for i, r := range ipt.rules[table][chain] {
		result[i] = r.Args()
	}
	return result
}

// ChainExists проверяет существование chain
func (ipt *FakeIPTables) ChainExists(table, chain string) bool {
	if ipt.rules[table] == nil {
		return false
	}
	_, exists := ipt.rules[table][chain]
	return exists
}

func (ipt *FakeIPTables) Proto() Protocol {
	return ipt.proto
}

func (ipt *FakeIPTables) Save() ([]byte, error) {
	buf := new(bytes.Buffer)

	tableNames := make([]string, 0, len(ipt.rules))
	for tableName := range ipt.rules {
		tableNames = append(tableNames, tableName)
	}
	slices.Sort(tableNames)

	for _, tableName := range tableNames {
		table := ipt.rules[tableName]
		buf.WriteByte('*')
		buf.WriteString(tableName)
		buf.WriteByte('\n')

		chainNames := make([]string, 0, len(table))
		for chainName := range table {
			chainNames = append(chainNames, chainName)
		}
		sort.Strings(chainNames)

		for _, chainName := range chainNames {
			buf.WriteByte(':')
			buf.WriteString(chainName)
			buf.WriteString(" - [0:0]\n")
		}
		for _, chainName := range chainNames {
			chainRules := table[chainName]
			for _, r := range chainRules {
				buf.WriteString("-A ")
				buf.WriteString(chainName)
				if len(r) > 0 {
					buf.WriteByte(' ')
					ruleWriteTo(buf, r)
				}
				buf.WriteByte('\n')
			}
		}
		buf.WriteString("COMMIT\n")
	}
	return buf.Bytes(), nil
}

func (ipt *FakeIPTables) Restore(data []byte) error {
	lines := bytes.Split(data, []byte("\n"))
	currentTable := ""
	for _, line := range lines {
		if len(line) == 0 {
			continue
		}

		switch line[0] {
		case '*': // Table declaration
			currentTable = string(line[1:])
			if ipt.rules[currentTable] == nil {
				ipt.rules[currentTable] = make(map[string][]Rule)
			}

		case ':': // Chain declaration
			fields := splitFields(line[1:])
			if len(fields) == 0 {
				return fmt.Errorf("invalid chain declaration")
			}
			chain := string(fields[0])
			if ipt.rules[currentTable][chain] == nil {
				ipt.rules[currentTable][chain] = []Rule{}
			}

		case '-': // Operation
			if currentTable == "" {
				return fmt.Errorf("rule outside of table")
			}

			fields := strings.Fields(string(line))
			if len(fields) < 2 {
				return fmt.Errorf("invalid rule: %q", line)
			}

			chain := fields[1]
			switch fields[0][1] {
			case 'A': // Append
				ipt.rules[currentTable][chain] = append(
					ipt.rules[currentTable][chain],
					ruleFromStrings(fields[2:]),
				)

			case 'I': // Insert
				if len(fields) < 3 {
					return fmt.Errorf("invalid insert Rule")
				}
				pos, err := strconv.Atoi(fields[2])
				if err != nil || pos < 1 {
					return fmt.Errorf("invalid insert position")
				}

				r := ruleFromStrings(fields[3:])
				chainRules := ipt.rules[currentTable][chain]

				if pos > len(chainRules)+1 {
					pos = len(chainRules) + 1
				}

				pos-- // 1-based → 0-based
				chainRules = append(chainRules[:pos],
					append([]Rule{r}, chainRules[pos:]...)...)

				ipt.rules[currentTable][chain] = chainRules

			case 'D': // Delete Rule
				r := ruleFromStrings(fields[2:])
				chainRules := ipt.rules[currentTable][chain]
				found := false

				for i, cr := range chainRules {
					if ruleEqual(cr, r) {
						ipt.rules[currentTable][chain] =
							append(chainRules[:i], chainRules[i+1:]...)
						found = true
						break
					}
				}

				if !found {
					return fmt.Errorf("Rule not found for delete: %v", fields[2:])
				}

			case 'F': // Flush chain
				ipt.rules[currentTable][chain] = []Rule{}

			case 'X': // Delete chain
				if len(ipt.rules[currentTable][chain]) != 0 {
					return fmt.Errorf("cannot delete non-empty chain %s", chain)
				}
				delete(ipt.rules[currentTable], chain)

			default:
				return fmt.Errorf("unknown iptables command %q", fields[0])
			}

		case '#': // Comment
			continue

		case 'C': // COMMIT
			currentTable = ""

		default:
			return fmt.Errorf("unknown iptables input %q", line)
		}
	}

	return nil
}
