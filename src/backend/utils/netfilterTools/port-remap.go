package netfilterTools

import (
	"errors"
	"fmt"
	"net"
	"strconv"
	"sync"
	"sync/atomic"

	"magitrickle/utils/iptables"

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

func (r *PortRemap) insertIPTablesRules(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}

	err := ipt.RegisterChainOverride("nat", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to create chain: %w", err)
	}

	for _, addr := range r.addresses {
		if !((ipt.Proto() == iptables.ProtocolIPv4 && len(addr.IP) == net.IPv4len) || (ipt.Proto() == iptables.ProtocolIPv6 && len(addr.IP) == net.IPv6len)) {
			continue
		}

		for _, iptablesArgs := range [][]string{
			{"-p", "tcp", "-d", addr.IP.String(), "--dport", strconv.Itoa(int(r.from)), "-j", "DNAT", "--to-destination", fmt.Sprintf(":%d", r.to)},
			{"-p", "udp", "-d", addr.IP.String(), "--dport", strconv.Itoa(int(r.from)), "-j", "DNAT", "--to-destination", fmt.Sprintf(":%d", r.to)},
		} {
			err = ipt.Append("nat", r.chainName, iptablesArgs...)
			if err != nil {
				return fmt.Errorf("failed to append rule: %w", err)
			}
		}
	}

	err = ipt.Insert("nat", "PREROUTING", 1, "-j", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to linking chain: %w", err)
	}

	err = ipt.Commit()
	if err != nil {
		return fmt.Errorf("failed to commit iptables rules: %w", err)
	}
	return nil
}

func (r *PortRemap) deleteIPTablesRules(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}
	var errs []error

	err := ipt.RegisterChainDelete("nat", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to clear chain: %w", err))
	}

	err = ipt.Delete("nat", "PREROUTING", "-j", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to unlinking chain: %w", err))
	}

	err = ipt.Commit()
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to commit iptables rules: %w", err))
	}
	return errors.Join(errs...)
}

func (r *PortRemap) enable() error {
	if !r.enabled.CompareAndSwap(false, true) {
		return nil
	}

	err := r.insertIPTablesRules(r.nh.IPTables4)
	if err != nil {
		return err
	}

	err = r.insertIPTablesRules(r.nh.IPTables6)
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

func (nh *Helper) PortRemap(name string, from, to uint16, addr []netlink.Addr) *PortRemap {
	return &PortRemap{
		nh:        nh,
		chainName: nh.ChainPrefix + name,
		addresses: addr,
		from:      from,
		to:        to,
	}
}
