package iptables

import (
	"bytes"
	"strings"
)

// Rule хранит части правила как []byte для минимизации аллокаций
type Rule [][]byte

// String реализует fmt.Stringer, возвращает правило как строку (части через пробел)
func (r Rule) String() string {
	return string(bytes.Join(r, []byte(" ")))
}

// Args возвращает правило как []string (для передачи в Append/Insert/Delete)
func (r Rule) Args() []string {
	result := make([]string, len(r))
	for i, part := range r {
		result[i] = string(part)
	}
	return result
}

// Contains проверяет, содержит ли правило подстроку
func (r Rule) Contains(substr string) bool {
	return strings.Contains(r.String(), substr)
}

// ruleEqual проверяет равенство двух правил
func ruleEqual(a, b Rule) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if !bytes.Equal(a[i], b[i]) {
			return false
		}
	}
	return true
}

// ruleFromStrings конвертирует []string в Rule ([][]byte)
func ruleFromStrings(s []string) Rule {
	r := make(Rule, len(s))
	for i, part := range s {
		r[i] = []byte(part)
	}
	return r
}

// ruleWriteTo записывает правило в буфер через пробелы
func ruleWriteTo(buf *bytes.Buffer, r Rule) {
	for i, part := range r {
		if i > 0 {
			buf.WriteByte(' ')
		}
		buf.Write(part)
	}
}

type option int8

const (
	optionAppend option = iota
	optionDelete
	optionInsert
	optionFlush
	optionDeleteChain
)

type command struct {
	Option  option
	Chain   []byte
	RuleNum int
	Rule    Rule
}
