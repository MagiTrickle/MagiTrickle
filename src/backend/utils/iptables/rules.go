package iptables

type option int8

const (
	optionAppend option = iota
	optionDelete
	optionInsert
	optionFlush
	optionDeleteChain
)

type rule []string

type command struct {
	Option  option
	Chain   string
	RuleNum int
	Rule    rule
}
