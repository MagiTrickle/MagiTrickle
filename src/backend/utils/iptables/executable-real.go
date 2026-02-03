package iptables

import (
	"bytes"
	"fmt"
	"os/exec"
)

type realIPTables struct {
	saveCmd    string
	restoreCmd string
	proto      Protocol
}

func NewRealIPTables() *realIPTables {
	return &realIPTables{
		saveCmd:    "iptables-save",
		restoreCmd: "iptables-restore",
		proto:      ProtocolIPv4,
	}
}

func NewRealIP6Tables() *realIPTables {
	return &realIPTables{
		saveCmd:    "ip6tables-save",
		restoreCmd: "ip6tables-restore",
		proto:      ProtocolIPv6,
	}
}

func (ipt *realIPTables) Proto() Protocol {
	return ipt.proto
}

func (ipt *realIPTables) Save() ([]byte, error) {
	cmd := exec.Command(ipt.saveCmd)

	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf(
			"iptables-save failed: %w: %s",
			err, stderr.String(),
		)
	}

	return stdout.Bytes(), nil
}

func (ipt *realIPTables) Restore(data []byte) error {
	cmd := exec.Command(ipt.restoreCmd, "--noflush")

	var stderr bytes.Buffer
	cmd.Stdin = bytes.NewReader(data)
	cmd.Stderr = &stderr

	if err := cmd.Run(); err != nil {
		return fmt.Errorf(
			"iptables-restore failed: %w: %s",
			err, stderr.String(),
		)
	}

	return nil
}
