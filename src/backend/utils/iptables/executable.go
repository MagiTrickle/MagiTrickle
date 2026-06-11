package iptables

type Executable interface {
	Save() ([]byte, error)
	Restore([]byte) error
	HasTable(table string) bool
	Proto() Protocol
}
