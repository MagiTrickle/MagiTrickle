package magitrickle

import (
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"magitrickle/models"
	"magitrickle/utils/netfilterTools"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
)

type Group struct {
	*models.Group

	enabled atomic.Bool
	locker  sync.Mutex

	app         *App
	ipset       *netfilterTools.IPSet
	ipsetToLink *netfilterTools.IPSetToLink
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

func (g *Group) Model() *models.Group {
	return g.Group
}

func (g *Group) addIPv4Subnet(subnet netfilterTools.IPv4Subnet, ttl netfilterTools.IPSetTimeout) error {
	return g.ipset.AddIPv4Subnet(subnet, ttl)
}

func (g *Group) AddIPv4Subnet(subnet netfilterTools.IPv4Subnet, ttl netfilterTools.IPSetTimeout) error {
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

func (g *Group) addIPv6Subnet(subnet netfilterTools.IPv6Subnet, ttl netfilterTools.IPSetTimeout) error {
	return g.ipset.AddIPv6Subnet(subnet, ttl)
}

func (g *Group) AddIPv6Subnet(subnet netfilterTools.IPv6Subnet, ttl netfilterTools.IPSetTimeout) error {
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

func (g *Group) delIPv4Subnet(subnet netfilterTools.IPv4Subnet) error {
	return g.ipset.DelIPv4Subnet(subnet)
}

func (g *Group) DelIPv4Subnet(subnet netfilterTools.IPv4Subnet) error {
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

func (g *Group) delIPv6Subnet(subnet netfilterTools.IPv6Subnet) error {
	return g.ipset.DelIPv6Subnet(subnet)
}

func (g *Group) DelIPv6Subnet(subnet netfilterTools.IPv6Subnet) error {
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

func (g *Group) listIPv4Subnets() (map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout, error) {
	return g.ipset.ListIPv4Subnets()
}

func (g *Group) ListIPv4Subnets() (map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout, error) {
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

func (g *Group) listIPv6Subnets() (map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout, error) {
	return g.ipset.ListIPv6Subnets()
}

func (g *Group) ListIPv6Subnets() (map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout, error) {
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

func (g *Group) sync() error {
	now := time.Now()
	newIPv4SubnetList := make(map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout)
	newIPv6SubnetList := make(map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout)
	knownDomains := g.app.recordsCache.ListKnownDomains()
	for _, domain := range g.Rules {
		if !domain.IsEnabled() {
			continue
		}
		switch domain.Type {
		case "subnet":
			ip, ipNet, err := net.ParseCIDR(domain.Rule)
			if err != nil {
				ip = net.ParseIP(domain.Rule)
				if ip == nil {
					continue
				}

				ip = ip.To4()
				if ip == nil {
					continue
				}

				ipNet = &net.IPNet{
					IP:   ip,
					Mask: net.CIDRMask(32, 32),
				}
			}

			ones, bits := ipNet.Mask.Size()
			if bits != 32 || ones > 32 {
				continue
			}

			var addr [4]byte
			copy(addr[:], ipNet.IP.Mask(ipNet.Mask).To4())
			cidr := uint8(ones)

			if addr == ([4]byte{}) && cidr == 0 {
				newIPv4SubnetList[netfilterTools.IPv4Subnet{
					Address: [4]byte{0x00},
					CIDR:    1,
				}] = nil
				newIPv4SubnetList[netfilterTools.IPv4Subnet{
					Address: [4]byte{0x80},
					CIDR:    1,
				}] = nil
			} else {
				newIPv4SubnetList[netfilterTools.IPv4Subnet{
					Address: addr,
					CIDR:    cidr,
				}] = nil
			}

		case "subnet6":
			ip, ipNet, err := net.ParseCIDR(domain.Rule)
			if err != nil {
				ip = net.ParseIP(domain.Rule)
				if ip == nil {
					continue
				}

				ip = ip.To16()
				if ip == nil {
					continue
				}

				ipNet = &net.IPNet{
					IP:   ip,
					Mask: net.CIDRMask(128, 128),
				}
			}

			ones, bits := ipNet.Mask.Size()
			if bits != 128 || ones > 128 {
				continue
			}

			var addr [16]byte
			copy(addr[:], ipNet.IP.Mask(ipNet.Mask).To16())
			cidr := uint8(ones)

			if addr == ([16]byte{}) && cidr == 0 {
				newIPv6SubnetList[netfilterTools.IPv6Subnet{
					Address: [16]byte{0x00},
					CIDR:    1,
				}] = nil
				newIPv6SubnetList[netfilterTools.IPv6Subnet{
					Address: [16]byte{0x80},
					CIDR:    1,
				}] = nil
			} else {
				newIPv6SubnetList[netfilterTools.IPv6Subnet{
					Address: addr,
					CIDR:    cidr,
				}] = nil
			}

		default:
			for _, domainName := range knownDomains {
				if !domain.IsMatch(domainName) {
					continue
				}
				domainAddresses := g.app.recordsCache.GetAddresses(domainName)
				for _, address := range domainAddresses {
					ttlDuration := address.Deadline.Sub(now).Seconds()
					if ttlDuration <= 0 {
						continue
					}
					ttl := uint32(ttlDuration)
					if len(address.Address) == net.IPv4len {
						subnet := netfilterTools.IPv4Subnet{Address: [4]byte(address.Address)}
						if oldTTL, exists := newIPv4SubnetList[subnet]; !exists || (oldTTL != nil && ttl > *oldTTL) {
							newIPv4SubnetList[subnet] = &ttl
						}
					} else if len(address.Address) == net.IPv6len {
						subnet := netfilterTools.IPv6Subnet{Address: [16]byte(address.Address)}
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
			log.Error().
				Err(err).
				Str("subnet", subnet.String()).
				Msg("failed to add subnet")
		} else {
			log.Debug().
				Str("subnet", subnet.String()).
				Msg("added subnet")
		}
	}
	for subnet := range oldIPv4SubnetList {
		if _, ok := newIPv4SubnetList[subnet]; ok {
			continue
		}

		if err := g.delIPv4Subnet(subnet); err != nil {
			log.Error().
				Err(err).
				Str("subnet", subnet.String()).
				Msg("failed to delete subnet")
		} else {
			log.Debug().
				Str("subnet", subnet.String()).
				Msg("deleted subnet")
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
			log.Error().
				Err(err).
				Str("subnet", subnet.String()).
				Msg("failed to add subnet")
		} else {
			log.Debug().
				Str("subnet", subnet.String()).
				Msg("added subnet")
		}
	}
	for subnet := range oldIPv6SubnetList {
		if _, ok := newIPv6SubnetList[subnet]; ok {
			continue
		}

		if err := g.delIPv6Subnet(subnet); err != nil {
			log.Error().
				Err(err).
				Str("subnet", subnet.String()).
				Msg("failed to delete subnet")
		} else {
			log.Debug().
				Str("subnet", subnet.String()).
				Msg("deleted subnet")
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

	return g.sync()
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
