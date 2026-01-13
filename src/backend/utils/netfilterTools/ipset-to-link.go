package netfilterTools

import (
	"errors"
	"fmt"
	"net"
	"strconv"
	"sync"
	"sync/atomic"

	"magitrickle/utils/iptables"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
	"github.com/vishvananda/netlink/nl"
	"golang.org/x/sys/unix"
)

const Blackhole = "blackhole"

type IPSetToLink struct {
	enabled atomic.Bool
	locker  sync.Mutex

	chainName string
	ifaceName string
	startIdx  uint32
	ipset     *IPSet
	nh        *Helper
	mark      uint32
	table     int
	ip4Rule   *netlink.Rule
	ip6Rule   *netlink.Rule
	ip4Route  [3]*netlink.Route
	ip6Route  [3]*netlink.Route
}

func (r *IPSetToLink) insertIPTablesRules(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}

	ipsetName := r.ipset.ipsetName
	if ipt.Proto() == iptables.ProtocolIPv4 {
		ipsetName += "_4"
	} else {
		ipsetName += "_6"
	}

	/*
		Filter Forward
	*/

	err := ipt.RegisterChainOverride("filter", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to create chain: %w", err)
	}

	if r.ifaceName != Blackhole {
		err = ipt.Append("filter", r.chainName, "-m", "set", "--match-set", ipsetName, "dst", "-o", r.ifaceName, "-j", "ACCEPT")
		if err != nil {
			return fmt.Errorf("failed to fix protect for IPv4: %w", err)
		}
	}

	err = ipt.Append("filter", "FORWARD", "-j", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to append rule to PREROUTING: %w", err)
	}

	/*
		Mangle Prerouting
	*/

	err = ipt.RegisterChainOverride("mangle", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to create chain: %w", err)
	}

	for _, iptablesArgs := range [][]string{
		{"-m", "set", "--match-set", ipsetName, "dst", "-j", "MARK", "--set-mark", strconv.Itoa(int(r.mark))},
		{"-m", "set", "--match-set", ipsetName, "dst", "-j", "CONNMARK", "--save-mark"},
	} {
		err = ipt.Append("mangle", r.chainName, iptablesArgs...)
		if err != nil {
			return fmt.Errorf("failed to append rule: %w", err)
		}
	}

	err = ipt.Append("mangle", "PREROUTING", "-j", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to append rule to PREROUTING: %w", err)
	}

	/*
		NAT Postrouting
	*/

	err = ipt.RegisterChainOverride("nat", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to create chain: %w", err)
	}

	err = ipt.Append("nat", r.chainName, "-m", "set", "--match-set", ipsetName, "dst", "-j", "MASQUERADE")
	if err != nil {
		return fmt.Errorf("failed to create rule: %w", err)
	}

	err = ipt.Append("nat", "POSTROUTING", "-j", r.chainName)
	if err != nil {
		return fmt.Errorf("failed to append rule to POSTROUTING: %w", err)
	}

	err = ipt.Commit()
	if err != nil {
		return fmt.Errorf("failed to commit iptables rules: %w", err)
	}
	return nil
}

func (r *IPSetToLink) deleteIPTablesRules(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}
	var errs []error

	/*
		Filter Forward
	*/

	err := ipt.RegisterChainDelete("filter", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to clear chain: %w", err))
	}

	err = ipt.Delete("filter", "FORWARD", "-j", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to unlinking chain: %w", err))
	}

	/*
		Mangle Prerouting
	*/

	err = ipt.RegisterChainDelete("mangle", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to delete chain: %w", err))
	}

	err = ipt.Delete("mangle", "PREROUTING", "-j", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to unlinking chain: %w", err))
	}

	/*
		NAT Postrouting
	*/

	err = ipt.RegisterChainDelete("nat", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to delete chain: %w", err))
	}

	err = ipt.Delete("nat", "POSTROUTING", "-j", r.chainName)
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to unlinking chain: %w", err))
	}

	err = ipt.Commit()
	if err != nil {
		errs = append(errs, fmt.Errorf("failed to commit iptables rules: %w", err))
	}
	return errors.Join(errs...)
}

func (r *IPSetToLink) insertIPRule() error {
	if r.nh.IPTables4 != nil {
		rule := netlink.NewRule()
		rule.Mark = r.mark
		rule.Table = r.table
		rule.Family = nl.FAMILY_V4
		_ = netlink.RuleDel(rule)
		err := netlink.RuleAdd(rule)
		if err != nil {
			return fmt.Errorf("error while mapping marked packages to table: %w", err)
		}
		r.ip4Rule = rule
	}

	if r.nh.IPTables6 != nil {
		rule := netlink.NewRule()
		rule.Mark = r.mark
		rule.Table = r.table
		rule.Family = nl.FAMILY_V6
		_ = netlink.RuleDel(rule)
		err := netlink.RuleAdd(rule)
		if err != nil {
			return fmt.Errorf("error while mapping marked packages to table: %w", err)
		}
		r.ip6Rule = rule
	}

	return nil
}

func (r *IPSetToLink) deleteIPRule() error {
	var errs []error

	if r.ip4Rule != nil {
		err := netlink.RuleDel(r.ip4Rule)
		if err != nil {
			errs = append(errs, fmt.Errorf("error while deleting rule: %w", err))
		}
		r.ip4Rule = nil
	}

	if r.ip6Rule != nil {
		err := netlink.RuleDel(r.ip6Rule)
		if err != nil {
			errs = append(errs, fmt.Errorf("error while deleting rule: %w", err))
		}
		r.ip6Rule = nil
	}

	return errors.Join(errs...)
}

func (r *IPSetToLink) insertIPRoute() error {
	var route *netlink.Route

	if r.nh.IPTables4 != nil {
		route = &netlink.Route{
			Dst:    &net.IPNet{IP: []byte{0, 0, 0, 0}, Mask: []byte{0, 0, 0, 0}},
			Table:  r.table,
			Type:   unix.RTN_BLACKHOLE,
			Family: nl.FAMILY_V4,
		}
		err := netlink.RouteAdd(route)
		if err != nil && !errors.Is(err, unix.EEXIST) {
			return fmt.Errorf("error while adding route: %w", err)
		}
		r.ip4Route[0] = route

		if r.ifaceName != Blackhole {
			iface, err := netlink.LinkByName(r.ifaceName)
			if err != nil {
				if errors.As(err, &netlink.LinkNotFoundError{}) {
					log.Warn().Str("iface", r.ifaceName).Msg("interface not found, it can be catched later")
					return nil
				}
				return fmt.Errorf("error while getting interface: %w", err)
			}
			if iface.Attrs().Flags&net.FlagUp == 0 {
				log.Warn().Str("iface", r.ifaceName).Msg("interface is down")
				return nil
			}

			route = &netlink.Route{
				LinkIndex: iface.Attrs().Index,
				Table:     r.table,
				Dst:       &net.IPNet{IP: []byte{0, 0, 0, 0}, Mask: []byte{128, 0, 0, 0}},
			}
			err = netlink.RouteAdd(route)
			if err != nil && !errors.Is(err, unix.EEXIST) {
				return fmt.Errorf("error while adding route: %w", err)
			}
			r.ip4Route[1] = route

			route = &netlink.Route{
				LinkIndex: iface.Attrs().Index,
				Table:     r.table,
				Dst:       &net.IPNet{IP: []byte{128, 0, 0, 0}, Mask: []byte{128, 0, 0, 0}},
			}
			err = netlink.RouteAdd(route)
			if err != nil && !errors.Is(err, unix.EEXIST) {
				return fmt.Errorf("error while adding route: %w", err)
			}
			r.ip4Route[2] = route
		}
	}

	if r.nh.IPTables6 != nil {
		route = &netlink.Route{
			Dst:    &net.IPNet{IP: []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, Mask: []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
			Table:  r.table,
			Type:   unix.RTN_BLACKHOLE,
			Family: nl.FAMILY_V6,
		}
		err := netlink.RouteAdd(route)
		if err != nil && !errors.Is(err, unix.EEXIST) {
			return fmt.Errorf("error while adding route: %w", err)
		}
		r.ip6Route[0] = route

		if r.ifaceName != Blackhole {
			iface, err := netlink.LinkByName(r.ifaceName)
			if err != nil {
				if errors.As(err, &netlink.LinkNotFoundError{}) {
					log.Warn().Str("iface", r.ifaceName).Msg("interface not found, it can be catched later")
					return nil
				}
				return fmt.Errorf("error while getting interface: %w", err)
			}
			if iface.Attrs().Flags&net.FlagUp == 0 {
				log.Warn().Str("iface", r.ifaceName).Msg("interface is down")
				return nil
			}

			route = &netlink.Route{
				LinkIndex: iface.Attrs().Index,
				Table:     r.table,
				Dst:       &net.IPNet{IP: []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, Mask: []byte{128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
				Family:    nl.FAMILY_V6,
			}
			err = netlink.RouteAdd(route)
			if err != nil && !errors.Is(err, unix.EEXIST) {
				return fmt.Errorf("error while adding route: %w", err)
			}
			r.ip6Route[1] = route

			route = &netlink.Route{
				LinkIndex: iface.Attrs().Index,
				Table:     r.table,
				Dst:       &net.IPNet{IP: []byte{128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, Mask: []byte{128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
				Family:    nl.FAMILY_V6,
			}
			err = netlink.RouteAdd(route)
			if err != nil && !errors.Is(err, unix.EEXIST) {
				return fmt.Errorf("error while adding route: %w", err)
			}
			r.ip6Route[2] = route
		}
	}

	return nil
}

func (r *IPSetToLink) deleteIPRoute() error {
	errs := make([]error, 0)

	for i := 2; i >= 0; i-- {
		if r.ip4Route[i] == nil {
			continue
		}
		err := netlink.RouteDel(r.ip4Route[i])
		if err != nil {
			errs = append(errs, fmt.Errorf("error while deleting route: %w", err))
		}
		r.ip4Route[i] = nil
	}

	for i := 2; i >= 0; i-- {
		if r.ip6Route[i] == nil {
			continue
		}
		err := netlink.RouteDel(r.ip6Route[i])
		if err != nil {
			errs = append(errs, fmt.Errorf("error while deleting route: %w", err))
		}
		r.ip6Route[i] = nil
	}

	return errors.Join(errs...)
}

func (r *IPSetToLink) getUnusedMarkAndTable() (idx uint32, err error) {
	// Find unused mark and table
	markMap := make(map[uint32]struct{})
	tableMap := map[int]struct{}{0: {}, 253: {}, 254: {}, 255: {}}

	rules, err := netlink.RuleList(nl.FAMILY_ALL)
	if err != nil {
		return 0, fmt.Errorf("error while getting rules: %w", err)
	}
	for _, rule := range rules {
		markMap[rule.Mark] = struct{}{}
		tableMap[rule.Table] = struct{}{}
	}

	routes, err := netlink.RouteListFiltered(nl.FAMILY_ALL, &netlink.Route{}, netlink.RT_FILTER_TABLE)
	if err != nil {
		return 0, fmt.Errorf("error while getting routes: %w", err)
	}
	for _, route := range routes {
		tableMap[route.Table] = struct{}{}
	}

	for idx = r.startIdx; idx < 0x7ffffffe; idx++ {
		if _, exists := tableMap[int(idx)]; !exists {
			break
		}
		if _, exists := markMap[idx]; !exists {
			break
		}
	}

	return idx, nil
}

func (r *IPSetToLink) enable() error {
	if !r.enabled.CompareAndSwap(false, true) {
		return nil
	}

	var err error
	idx, err := r.getUnusedMarkAndTable()
	if err != nil {
		return err
	}
	r.mark, r.table = idx, int(idx)

	err = r.insertIPRule()
	if err != nil {
		return err
	}

	err = r.insertIPRoute()
	if err != nil {
		return err
	}

	err = r.insertIPTablesRules(r.nh.IPTables4)
	if err != nil {
		return err
	}

	err = r.insertIPTablesRules(r.nh.IPTables6)
	if err != nil {
		return err
	}

	return nil
}

func (r *IPSetToLink) Enable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	err := r.enable()
	if err != nil {
		r.disable()
	} else {
		log.Debug().
			Int("table", r.table).
			Int("mark", int(r.mark)).
			Msg("using ip table and mark")
	}

	return err
}

func (r *IPSetToLink) disable() error {
	if !r.enabled.Load() {
		return nil
	}
	defer r.enabled.Store(false)

	var errs []error
	errs = append(errs, r.deleteIPRoute())
	errs = append(errs, r.deleteIPRule())
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables4))
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables6))
	return errors.Join(errs...)
}

func (r *IPSetToLink) Disable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	return r.disable()
}

func (r *IPSetToLink) ClearIfDisabled() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if r.enabled.Load() {
		return nil
	}

	var errs []error
	errs = append(errs, r.deleteIPRoute())
	errs = append(errs, r.deleteIPRule())
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables4))
	errs = append(errs, r.deleteIPTablesRules(r.nh.IPTables6))
	return errors.Join(errs...)
}

func (r *IPSetToLink) LinkUpdateHook(event netlink.LinkUpdate) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() || event.Link.Attrs().Name != r.ifaceName {
		return nil
	}

	var errs []error
	errs = append(errs, r.insertIPRoute())
	return errors.Join(errs...)
}

func (nh *Helper) IPSetToLink(name string, ifaceName string, ipset *IPSet) *IPSetToLink {
	return &IPSetToLink{
		nh:        nh,
		chainName: nh.ChainPrefix + name,
		ifaceName: ifaceName,
		ipset:     ipset,
		startIdx:  nh.StartIdx,
	}
}
