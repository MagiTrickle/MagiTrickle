package iptables

type priority int8

type chain interface {
	Compile(chainName []byte, existedRules []Rule) ([]command, priority, error)
	Append(rule Rule) error
	Insert(ruleNum int, rule Rule) error
	Delete(rule Rule) error
}
