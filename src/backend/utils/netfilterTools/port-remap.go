package netfilterTools

import (
	"errors"
	"fmt"
	"net"
	"strconv"
	"sync"
	"sync/atomic"

	"github.com/coreos/go-iptables/iptables"
	"github.com/vishvananda/netlink"
)

type PortRemap struct {
	enabled atomic.Bool
	locker  sync.Mutex

	chainName string
	addresses []netlink.Addr
	from      uint16
	to        uint16
	nh        *Helper
}

func (r *PortRemap) insertIPTablesRules(ipt *iptables.IPTables, table string) error {
	if ipt == nil {
		return nil
	}
	if table == "" || table == "nat" {
		err := ipt.NewChain("nat", r.chainName)
		if err != nil {
			// If not "AlreadyExists"
			if eerr, eok := err.(*iptables.Error); !(eok && eerr.ExitStatus() == 1) {
				return fmt.Errorf("failed to create chain: %w", err)
			}
		}

		for _, addr := range r.addresses {
			if !((ipt.Proto() == iptables.ProtocolIPv4 && len(addr.IP) == net.IPv4len) || (ipt.Proto() == iptables.ProtocolIPv6 && len(addr.IP) == net.IPv6len)) {
				continue
			}

			for _, iptablesArgs := range [][]string{
				{"-p", "tcp", "-d", addr.IP.String(), "--dport", strconv.Itoa(int(r.from)), "-j", "DNAT", "--to-destination", fmt.Sprintf(":%d", r.to)},
				{"-p", "udp", "-d", addr.IP.String(), "--dport", strconv.Itoa(int(r.from)), "-j", "DNAT", "--to-destination", fmt.Sprintf(":%d", r.to)},
			} {
				err = ipt.AppendUnique("nat", r.chainName, iptablesArgs...)
				if err != nil {
					return fmt.Errorf("failed to append rule: %w", err)
				}
			}
		}

		err = ipt.InsertUnique("nat", "PREROUTING", 1, "-j", r.chainName)
		if err != nil {
			return fmt.Errorf("failed to linking chain: %w", err)
		}
	}

	return nil
}

func (r *PortRemap) deleteIPTablesRules(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}
	var errs []error

	err := ipt.ClearChain("nat", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to clear chain: %w", err))
	}

	err = ipt.DeleteIfExists("nat", "PREROUTING", "-j", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to unlinking chain: %w", err))
	}

	err = ipt.DeleteChain("nat", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to delete chain: %w", err))
	}

	return errors.Join(errs...)
}

func (r *PortRemap) enable() error {
	if !r.enabled.CompareAndSwap(false, true) {
		return nil
	}

	err := r.deleteIPTablesRules(r.nh.IPTables4)
	if err != nil {
		return err
	}

	err = r.insertIPTablesRules(r.nh.IPTables4, "")
	if err != nil {
		return err
	}

	err = r.deleteIPTablesRules(r.nh.IPTables6)
	if err != nil {
		return err
	}

	err = r.insertIPTablesRules(r.nh.IPTables6, "")
	if err != nil {
		return err
	}

	return nil
}

func (r *PortRemap) Enable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	err := r.enable()
	if err != nil {
		r.disable()
	}

	return err
}

func (r *PortRemap) disable() error {
	if !r.enabled.Load() {
		return nil
	}
	defer r.enabled.Store(false)

	var errs []error
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables4))
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables6))
	return errors.Join(errs...)
}

func (r *PortRemap) Disable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	return r.disable()
}

func (r *PortRemap) NetfilterDHook(iptType, table string) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil
	}

	if iptType == "" || iptType == "iptables" {
		err := r.insertIPTablesRules(r.nh.IPTables4, table)
		if err != nil {
			return err
		}
	}
	if iptType == "" || iptType == "ip6tables" {
		err := r.insertIPTablesRules(r.nh.IPTables6, table)
		if err != nil {
			return err
		}
	}

	return nil
}

func (nh *Helper) PortRemap(name string, from, to uint16, addr []netlink.Addr) *PortRemap {
	return &PortRemap{
		nh:        nh,
		chainName: nh.ChainPrefix + name,
		addresses: addr,
		from:      from,
		to:        to,
	}
}
