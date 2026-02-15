package iptables

import (
	"bytes"
	"errors"
	"fmt"
	"slices"
	"strconv"
	"sync"
	"time"

	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"
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

func (ipt *IPTables) RegisterChainPatch(table, chainName string) error {
	ipt.sync.Lock()
	defer ipt.sync.Unlock()

	if _, ok := ipt.rules[table]; !ok {
		ipt.rules[table] = make(map[string]chain)
	}
	ipt.rules[table][chainName] = &chainPatch{}
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

func (ipt *IPTables) Append(table, chain string, ruleArgs ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Append(ruleFromStrings(ruleArgs))
}

func (ipt *IPTables) Insert(table, chain string, ruleNum int, ruleArgs ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Insert(ruleNum, ruleFromStrings(ruleArgs))
}

func (ipt *IPTables) Delete(table, chain string, ruleArgs ...string) error {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	if ipt.rules[table] == nil || ipt.rules[table][chain] == nil {
		return ErrChainNotInitialized
	}

	return ipt.rules[table][chain].Delete(ruleFromStrings(ruleArgs))
}

func (ipt *IPTables) GetCurrentRules() (map[string]map[string][]Rule, error) {
	rules := make(map[string]map[string][]Rule)

	data, err := ipt.executable.Save()
	if err != nil {
		return nil, err
	}

	lines := bytes.Split(data, []byte("\n"))
	var currentTable string
	for _, line := range lines {
		if len(line) == 0 {
			continue
		}

		switch line[0] {
		case '*':
			currentTable = string(line[1:])
			if rules[currentTable] == nil {
				rules[currentTable] = make(map[string][]Rule)
			}

		case ':':
			// Формат: ":CHAIN POLICY [packets:bytes]"
			// Найдём первый пробел для получения имени chain
			spaceIdx := bytes.IndexByte(line[1:], ' ')
			var chainName string
			if spaceIdx == -1 {
				chainName = string(line[1:])
			} else {
				chainName = string(line[1 : 1+spaceIdx])
			}
			if chainName == "" {
				return nil, fmt.Errorf("invalid chain declaration")
			}
			if rules[currentTable][chainName] == nil {
				rules[currentTable][chainName] = []Rule{}
			}

		case '-':
			if currentTable == "" {
				return nil, fmt.Errorf("rule outside of table")
			}

			fields := splitFields(line)
			if len(fields) < 2 {
				return nil, fmt.Errorf("invalid rule: %q", line)
			}

			chainName := string(fields[1])
			if len(fields[0]) < 2 {
				return nil, fmt.Errorf("invalid rule command: %q", fields[0])
			}
			switch fields[0][1] {
			case 'A': // Append
				// fields[2:] - правило без оператора
				rules[currentTable][chainName] = append(rules[currentTable][chainName], fields[2:])

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

// splitFields разбивает строку на поля по пробелам, возвращая срезы оригинального буфера
func splitFields(data []byte) [][]byte {
	var fields [][]byte
	start := -1
	for i, b := range data {
		if b == ' ' || b == '\t' {
			if start >= 0 {
				fields = append(fields, data[start:i])
				start = -1
			}
		} else {
			if start < 0 {
				start = i
			}
		}
	}
	if start >= 0 {
		fields = append(fields, data[start:])
	}
	return fields
}

func (ipt *IPTables) Commit() (err error) {
	ipt.sync.RLock()
	defer ipt.sync.RUnlock()

	buf := new(bytes.Buffer)
	start := time.Now()
	defer func() {
		elapsed := time.Since(start)
		iptType := "iptables"
		if ipt.Proto() == ProtocolIPv6 {
			iptType = "ip6tables"
		}
		var logEvent *zerolog.Event
		if err != nil {
			logEvent = log.Error().Err(err)
		} else {
			logEvent = log.Trace()
		}
		logEvent.Dur("elapsed_ms", elapsed).Str("type", iptType).Bytes("buf", buf.Bytes()).Msg("iptables commit")
	}()

	curRules, err := ipt.GetCurrentRules()
	if err != nil {
		return err
	}

	for tableName, table := range ipt.rules {
		curTable := curRules[tableName]

		var tableExists bool
		tableNameBytes := []byte(tableName)

		commandsPrioritized := make(map[int8][]command)
		for chainName, chain := range table {
			chainNameBytes := []byte(chainName)
			var curChain []Rule
			if curTable[chainName] != nil {
				curChain = curTable[chainName]
			}
			commands, priority, err := chain.Compile(chainNameBytes, curChain)
			if err != nil {
				return err
			}
			if commandsPrioritized[int8(priority)] == nil {
				commandsPrioritized[int8(priority)] = make([]command, 0)
			}
			if len(commands) > 0 {
				if !tableExists {
					buf.WriteByte('*')
					buf.Write(tableNameBytes)
					buf.WriteByte('\n')
					tableExists = true
				}
				buf.WriteByte(':')
				buf.Write(chainNameBytes)
				buf.WriteString(" - [0:0]\n")
				commandsPrioritized[int8(priority)] = append(commandsPrioritized[int8(priority)], commands...)
			}
		}

		if !tableExists {
			continue
		}

		priorities := make([]int8, 0, len(commandsPrioritized))
		for priority := range commandsPrioritized {
			priorities = append(priorities, priority)
		}
		slices.Sort(priorities)

		for _, priority := range priorities {
			for _, cmd := range commandsPrioritized[priority] {
				switch cmd.Option {
				case optionAppend:
					buf.WriteString("-A ")
					buf.Write(cmd.Chain)
					if len(cmd.Rule) > 0 {
						buf.WriteByte(' ')
						ruleWriteTo(buf, cmd.Rule)
					}
					buf.WriteByte('\n')
				case optionDelete:
					buf.WriteString("-D ")
					buf.Write(cmd.Chain)
					if len(cmd.Rule) > 0 {
						buf.WriteByte(' ')
						ruleWriteTo(buf, cmd.Rule)
					}
					buf.WriteByte('\n')
				case optionInsert:
					buf.WriteString("-I ")
					buf.Write(cmd.Chain)
					buf.WriteByte(' ')
					buf.WriteString(strconv.Itoa(cmd.RuleNum))
					if len(cmd.Rule) > 0 {
						buf.WriteByte(' ')
						ruleWriteTo(buf, cmd.Rule)
					}
					buf.WriteByte('\n')
				case optionFlush:
					buf.WriteString("-F ")
					buf.Write(cmd.Chain)
					buf.WriteByte('\n')
				case optionDeleteChain:
					buf.WriteString("-X ")
					buf.Write(cmd.Chain)
					buf.WriteByte('\n')
				}
			}
		}
		buf.WriteString("COMMIT\n")
	}

	if buf.Len() == 0 {
		return nil
	}

	return ipt.executable.Restore(buf.Bytes())
}
