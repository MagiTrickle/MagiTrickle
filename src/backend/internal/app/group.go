package app

import (
	"errors"
	"fmt"
	"net"
	"regexp"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"magitrickle/models"
	netfilterHelper "magitrickle/netfilter-helper"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
)

var (
	ipv4SubnetRe = regexp.MustCompile(`^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:/(\d{1,2}))?$`)
)

type Group struct {
	*models.Group

	enabled atomic.Bool
	locker  sync.Mutex

	app         *App
	ipset       *netfilterHelper.IPSet
	ipsetToLink *netfilterHelper.IPSetToLink
}

func (g *Group) Enabled() bool {
	return g.enabled.Load()
}

func NewGroup(group *models.Group, app *App) (*Group, error) {
	return &Group{
		Group: group,
		app:   app,
	}, nil
}

func (g *Group) addIPv4Subnet(subnet netfilterHelper.IPv4Subnet, ttl netfilterHelper.IPSetTimeout) error {
	return g.ipset.AddIPv4Subnet(subnet, ttl)
}

func (g *Group) AddIPv4Subnet(subnet netfilterHelper.IPv4Subnet, ttl netfilterHelper.IPSetTimeout) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.addIPv4Subnet(subnet, ttl)
}

func (g *Group) addIPv6Subnet(subnet netfilterHelper.IPv6Subnet, ttl netfilterHelper.IPSetTimeout) error {
	return g.ipset.AddIPv6Subnet(subnet, ttl)
}

func (g *Group) AddIPv6Subnet(subnet netfilterHelper.IPv6Subnet, ttl netfilterHelper.IPSetTimeout) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.addIPv6Subnet(subnet, ttl)
}

func (g *Group) delIPv4Subnet(subnet netfilterHelper.IPv4Subnet) error {
	return g.ipset.DelIPv4Subnet(subnet)
}

func (g *Group) DelIPv4Subnet(subnet netfilterHelper.IPv4Subnet) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.delIPv4Subnet(subnet)
}

func (g *Group) delIPv6Subnet(subnet netfilterHelper.IPv6Subnet) error {
	return g.ipset.DelIPv6Subnet(subnet)
}

func (g *Group) DelIPv6Subnet(subnet netfilterHelper.IPv6Subnet) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.delIPv6Subnet(subnet)
}

func (g *Group) listIPv4Subnets() (map[netfilterHelper.IPv4Subnet]netfilterHelper.IPSetTimeout, error) {
	return g.ipset.ListIPv4Subnets()
}

func (g *Group) ListIPv4Subnets() (map[netfilterHelper.IPv4Subnet]netfilterHelper.IPSetTimeout, error) {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil, nil
	}

	if !g.Group.Enable {
		return nil, nil
	}

	return g.listIPv4Subnets()
}

func (g *Group) listIPv6Subnets() (map[netfilterHelper.IPv6Subnet]netfilterHelper.IPSetTimeout, error) {
	return g.ipset.ListIPv6Subnets()
}

func (g *Group) ListIPv6Subnets() (map[netfilterHelper.IPv6Subnet]netfilterHelper.IPSetTimeout, error) {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil, nil
	}

	if !g.Group.Enable {
		return nil, nil
	}

	return g.listIPv6Subnets()
}

func (g *Group) enable() error {
	if !g.enabled.CompareAndSwap(false, true) {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	ipset := g.app.nfHelper.IPSet(g.ID.String())
	ipsetToLink := g.app.nfHelper.IPSetToLink(g.ID.String(), g.Interface, ipset)
	if err := ipsetToLink.ClearIfDisabled(); err != nil {
		return fmt.Errorf("failed to clear iptables: %w", err)
	}

	if err := ipset.Enable(); err != nil {
		return fmt.Errorf("failed to initialize ipset: %w", err)
	}
	g.ipset = ipset

	if err := ipsetToLink.Enable(); err != nil {
		return fmt.Errorf("failed to link ipset to interface: %w", err)
	}
	g.ipsetToLink = ipsetToLink

	return nil
}

func (g *Group) Enable() error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if err := g.enable(); err != nil {
		_ = g.disable()
		return err
	}
	return nil
}

func (g *Group) disable() error {
	if !g.Enabled() {
		return nil
	}
	defer g.enabled.Store(false)

	if !g.Group.Enable {
		return nil
	}

	var errs []error
	errs = append(errs, func() error {
		if g.ipsetToLink == nil {
			return nil
		}
		if err := g.ipsetToLink.Disable(); err != nil {
			return fmt.Errorf("failed to unlink ipset from interface: %w", err)
		}
		g.ipsetToLink = nil
		return nil
	}())
	errs = append(errs, func() error {
		if g.ipset == nil {
			return nil
		}
		if err := g.ipset.Disable(); err != nil {
			return fmt.Errorf("failed to destroy ipset: %w", err)
		}
		g.ipset = nil
		return nil
	}())
	return errors.Join(errs...)
}

func (g *Group) Disable() error {
	g.locker.Lock()
	defer g.locker.Unlock()
	return g.disable()
}

func (g *Group) syncSubnets() error {
	now := time.Now()
	newIPv4SubnetList := make(map[netfilterHelper.IPv4Subnet]netfilterHelper.IPSetTimeout)
	newIPv6SubnetList := make(map[netfilterHelper.IPv6Subnet]netfilterHelper.IPSetTimeout)
	knownDomains := g.app.records.ListKnownDomains()
RuleLoop:
	for _, domain := range g.Rules {
		if !domain.IsEnabled() {
			continue
		}
		switch domain.Type {
		case "subnet":
			matches := ipv4SubnetRe.FindStringSubmatch(domain.Rule)
			if matches == nil {
				continue
			}

			var addr [4]byte
			for i := 1; i <= 4; i++ {
				n, _ := strconv.Atoi(matches[i])
				if n > 255 {
					continue RuleLoop
				}

				addr[i-1] = uint8(n)
			}

			var cidr uint8
			if matches[5] != "" {
				n, _ := strconv.Atoi(matches[5])
				if n > 32 {
					continue RuleLoop
				}

				cidr = uint8(n)
				addr = [4]byte(net.IP(addr[:]).Mask(net.CIDRMask(n, 32)))
			}

			if cidr != 0 {
				newIPv4SubnetList[netfilterHelper.IPv4Subnet{
					Address: addr,
					CIDR:    cidr,
				}] = nil
			} else {
				// Processing 0.0.0.0/0
				newIPv4SubnetList[netfilterHelper.IPv4Subnet{
					Address: [4]byte{0, addr[1], addr[2], addr[3]},
					CIDR:    1,
				}] = nil
				newIPv4SubnetList[netfilterHelper.IPv4Subnet{
					Address: [4]byte{128, addr[1], addr[2], addr[3]},
					CIDR:    1,
				}] = nil
			}

		default:
			for _, domainName := range knownDomains {
				if !domain.IsMatch(domainName) {
					continue
				}
				domainAddresses := g.app.records.GetAddresses(domainName)
				for _, address := range domainAddresses {
					ttl := uint32(now.Sub(address.Deadline).Seconds())
					if len(address.Address) == net.IPv4len {
						subnet := netfilterHelper.IPv4Subnet{Address: [4]byte(address.Address)}
						if oldTTL, exists := newIPv4SubnetList[subnet]; !exists || (oldTTL != nil && ttl > *oldTTL) {
							newIPv4SubnetList[subnet] = &ttl
						}
					} else {
						subnet := netfilterHelper.IPv6Subnet{Address: [16]byte(address.Address)}
						if oldTTL, exists := newIPv6SubnetList[subnet]; !exists || (oldTTL != nil && ttl > *oldTTL) {
							newIPv6SubnetList[subnet] = &ttl
						}
					}
				}
			}
		}
	}

	oldIPv4SubnetList, err := g.listIPv4Subnets()
	if err != nil {
		return fmt.Errorf("failed to get old ipset list: %w", err)
	}
	for subnet, newTTL := range newIPv4SubnetList {
		if oldTTL, ok := oldIPv4SubnetList[subnet]; ok {
			if oldTTL == nil || (newTTL != nil && *newTTL < *oldTTL) {
				continue
			}
		}

		if err := g.addIPv4Subnet(subnet, newTTL); err != nil {
			log.Error().Str("subnet", subnet.String()).Err(err).Msg("failed to add subnet")
		} else {
			log.Trace().Str("subnet", subnet.String()).Msg("added subnet")
		}
	}
	for subnet := range oldIPv4SubnetList {
		if _, ok := newIPv4SubnetList[subnet]; ok {
			continue
		}

		if err := g.delIPv4Subnet(subnet); err != nil {
			log.Error().Str("subnet", subnet.String()).Err(err).Msg("failed to delete subnet")
		} else {
			log.Trace().Str("subnet", subnet.String()).Msg("deleted subnet")
		}
	}

	oldIPv6SubnetList, err := g.listIPv6Subnets()
	if err != nil {
		return fmt.Errorf("failed to get old ipset list: %w", err)
	}
	for subnet, newTTL := range newIPv6SubnetList {
		if oldTTL, ok := oldIPv6SubnetList[subnet]; ok {
			if oldTTL == nil || (newTTL != nil && *newTTL < *oldTTL) {
				continue
			}
		}

		if err := g.addIPv6Subnet(subnet, newTTL); err != nil {
			log.Error().Str("subnet", subnet.String()).Err(err).Msg("failed to add subnet")
		} else {
			log.Trace().Str("subnet", subnet.String()).Msg("added subnet")
		}
	}
	for subnet := range oldIPv6SubnetList {
		if _, ok := newIPv6SubnetList[subnet]; ok {
			continue
		}

		if err := g.delIPv6Subnet(subnet); err != nil {
			log.Error().Str("subnet", subnet.String()).Err(err).Msg("failed to delete subnet")
		} else {
			log.Trace().Str("subnet", subnet.String()).Msg("deleted subnet")
		}
	}

	return nil
}

func (g *Group) Sync() error {
	g.locker.Lock()
	defer g.locker.Unlock()

	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.syncSubnets()
}

func (g *Group) NetfilterDHook(iptType, table string) error {
	g.locker.Lock()
	defer g.locker.Unlock()

	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.ipsetToLink.NetfilterDHook(iptType, table)
}

func (g *Group) LinkUpdateHook(event netlink.LinkUpdate) error {
	g.locker.Lock()
	defer g.locker.Unlock()

	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.ipsetToLink.LinkUpdateHook(event)
}
