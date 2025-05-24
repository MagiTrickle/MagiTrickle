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
	ipnetRe = regexp.MustCompile(`^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:/(\d{0,2}))?$`)
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

func (g *Group) addIPNet(address net.IP, cidr uint8, ttl uint32) error {
	return g.ipset.AddIPNet(address, cidr, &ttl)
}

func (g *Group) AddIP(address net.IP, cidr uint8, ttl uint32) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.addIPNet(address, cidr, ttl)
}

func (g *Group) delIPNet(address net.IP, cidr uint8) error {
	return g.ipset.DelIPNet(address, cidr)
}

func (g *Group) DelIP(address net.IP, cidr uint8) error {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	return g.delIPNet(address, cidr)
}

func (g *Group) listIPNets() (map[string]*uint32, error) {
	return g.ipset.ListIPNets()
}

func (g *Group) ListIPNets() (map[string]*uint32, error) {
	g.locker.Lock()
	defer g.locker.Unlock()
	if !g.Enabled() {
		return nil, nil
	}

	if !g.Group.Enable {
		return nil, nil
	}

	return g.listIPNets()
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

func (g *Group) Sync() error {
	g.locker.Lock()
	defer g.locker.Unlock()

	if !g.Enabled() {
		return nil
	}

	if !g.Group.Enable {
		return nil
	}

	now := time.Now()
	addresses := make(map[string]uint32)
	knownDomains := g.app.records.ListKnownDomains()
	for _, domain := range g.Rules {
		if !domain.IsEnabled() {
			continue
		}
		switch domain.Type {
		case "ipnet":
			matches := ipnetRe.FindStringSubmatch(domain.Rule)
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

			addresses[string(addr)] = 0
		default:
			for _, domainName := range knownDomains {
				if !domain.IsMatch(domainName) {
					continue
				}
				domainAddresses := g.app.records.GetARecords(domainName)
				for _, address := range domainAddresses {
					ttl := uint32(now.Sub(address.Deadline).Seconds())
					addressCIDR := append(address.Address, 0)
					if oldTTL, ok := addresses[string(addressCIDR)]; !ok || ttl > oldTTL {
						addresses[string(addressCIDR)] = ttl
					}
				}
			}
		}
	}
	currentAddresses, err := g.listIPNets()
	if err != nil {
		return fmt.Errorf("failed to get old ipset list: %w", err)
	}
	for addr, ttl := range addresses {
		if currTTL, exists := currentAddresses[addr]; exists {
			if currTTL == nil {
				continue
			} else {
				if ttl < *currTTL {
					continue
				}
			}
		}
		ip := net.IP(addr[:len(addr)-1])
		cidr := addr[len(addr)-1]
		if err := g.addIPNet(ip, cidr, ttl); err != nil {
			log.Error().Str("address", ip.String()).Err(err).Msg("failed to add address")
		} else {
			log.Trace().Str("address", ip.String()).Msg("added address")
		}
	}
	for addr := range currentAddresses {
		if _, ok := addresses[addr]; ok {
			continue
		}
		ip := net.IP(addr)
		cidr := addr[len(addr)-1]
		if err := g.delIPNet(ip, cidr); err != nil {
			log.Error().Str("address", ip.String()).Err(err).Msg("failed to delete address")
		} else {
			log.Trace().Str("address", ip.String()).Msg("deleted address")
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
