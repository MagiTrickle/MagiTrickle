package netfilterHelper

import (
	"errors"
	"fmt"
	"os"
	"sync"
	"sync/atomic"

	"github.com/vishvananda/netlink"
	"golang.org/x/sys/unix"
)

type IPv4Subnet struct {
	Address [4]byte
	CIDR    uint8
}

func (subnet IPv4Subnet) String() string {
	if subnet.CIDR == 0 {
		return fmt.Sprintf("%d.%d.%d.%d", subnet.Address[0], subnet.Address[1], subnet.Address[2], subnet.Address[3])
	} else {
		return fmt.Sprintf("%d.%d.%d.%d/%d", subnet.Address[0], subnet.Address[1], subnet.Address[2], subnet.Address[3], subnet.CIDR)
	}
}

type IPSetTimeout *uint32

type IPSet struct {
	enabled atomic.Bool
	locker  sync.Mutex

	ipsetName string
}

func (r *IPSet) AddIPv4Subnet(subnet IPv4Subnet, timeout IPSetTimeout) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil
	}

	err := netlink.IpsetAdd(r.ipsetName+"_4", &netlink.IPSetEntry{
		IP:      subnet.Address[:],
		CIDR:    subnet.CIDR,
		Timeout: timeout,
		Replace: true,
	})
	if err != nil {
		return fmt.Errorf("failed to add address: %w", err)
	}

	return nil
}

func (r *IPSet) DelIPv4Subnet(subnet IPv4Subnet) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil
	}

	err := netlink.IpsetDel(r.ipsetName+"_4", &netlink.IPSetEntry{
		IP:   subnet.Address[:],
		CIDR: subnet.CIDR,
	})
	if err != nil {
		return fmt.Errorf("failed to delete address: %w", err)
	}

	return nil
}

func (r *IPSet) ListIPv4Subnets() (map[IPv4Subnet]IPSetTimeout, error) {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil, nil
	}

	addresses := make(map[IPv4Subnet]IPSetTimeout)

	list, err := netlink.IpsetList(r.ipsetName + "_4")
	if err != nil {
		return nil, err
	}
	for _, entry := range list.Entries {
		subnet := IPv4Subnet{
			Address: [4]byte(entry.IP),
			CIDR:    entry.CIDR,
		}
		addresses[subnet] = entry.Timeout
	}

	return addresses, nil
}

func (r *IPSet) ipsetCreate() error {
	err := netlink.IpsetCreate(r.ipsetName+"_4", "hash:net", netlink.IpsetCreateOptions{
		Timeout: func(i uint32) *uint32 { return &i }(300),
		Family:  unix.AF_INET,
	})
	if err != nil {
		return fmt.Errorf("failed to create ipset: %w", err)
	}

	err = netlink.IpsetCreate(r.ipsetName+"_6", "hash:net", netlink.IpsetCreateOptions{
		Timeout: func(i uint32) *uint32 { return &i }(300),
		Family:  unix.AF_INET6,
	})
	if err != nil {
		return fmt.Errorf("failed to create ipset: %w", err)
	}

	return nil
}

func (r *IPSet) ipsetDestroy() error {
	var errs []error
	err := netlink.IpsetDestroy(r.ipsetName + "_4")
	if err != nil && !os.IsNotExist(err) {
		errs = append(errs, err)
	}
	err = netlink.IpsetDestroy(r.ipsetName + "_6")
	if err != nil && !os.IsNotExist(err) {
		errs = append(errs, err)
	}
	if errs != nil {
		return fmt.Errorf("failed to destroy ipsets: %w", errors.Join(errs...))
	}
	return nil
}

func (r *IPSet) enable() error {
	if !r.enabled.CompareAndSwap(false, true) {
		return nil
	}

	err := r.ipsetDestroy()
	if err != nil {
		return err
	}

	err = r.ipsetCreate()
	if err != nil {
		return err
	}

	return nil
}

func (r *IPSet) Enable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	err := r.enable()
	if err != nil {
		r.disable()
	}

	return err
}

func (r *IPSet) disable() error {
	if !r.enabled.Load() {
		return nil
	}
	defer r.enabled.Store(false)

	return r.ipsetDestroy()
}

func (r *IPSet) Disable() error {
	r.locker.Lock()
	defer r.locker.Unlock()

	return r.disable()
}

func (nh *NetfilterHelper) IPSet(name string) *IPSet {
	return &IPSet{
		ipsetName: nh.IpsetPrefix + name,
	}
}
