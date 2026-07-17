package iptables

type Executable interface {
	Save() ([]byte, error)
	Restore([]byte) error
	Proto() Protocol
}
