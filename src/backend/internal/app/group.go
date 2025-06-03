package app

import (
	"errors"
	"fmt"
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
	ipv4SubnetRe = regexp.MustCompile(`^(\d+)\.(\d+)\.(\d+)\.(\d+)(?:/(\d+))?$`)
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

func (g *Group) SyncIPv4Subnets() error {
	g.locker.Lock()
	defer g.locker.Unlock()

	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	now := time.Now()
	newIPv4SubnetList := make(map[netfilterHelper.IPv4Subnet]netfilterHelper.IPSetTimeout)
	knownDomains := g.app.records.ListKnownDomains()
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

			addr := make([]byte, 5)
			for i := 1; i <= 5; i++ {
				if matches[i] == "" {
					continue
				}

				n, _ := strconv.Atoi(matches[i])
				addr[i-1] = uint8(n)
			}

			// TODO: Validating of subnet

			if !(addr[0] == 0 && matches[5] == "0") {
				newIPv4SubnetList[netfilterHelper.IPv4Subnet{
					Address: [4]byte(addr),
					CIDR:    addr[4],
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
				domainAddresses := g.app.records.GetARecords(domainName)
				for _, address := range domainAddresses {
					ttl := uint32(now.Sub(address.Deadline).Seconds())
					subnet := netfilterHelper.IPv4Subnet{Address: [4]byte(address.Address)}
					if oldTTL, exists := newIPv4SubnetList[subnet]; !exists || (oldTTL != nil && ttl > *oldTTL) {
						newIPv4SubnetList[subnet] = &ttl
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
	return nil
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
