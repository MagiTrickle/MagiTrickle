package netfilterTools

import (
	"errors"
	"fmt"
	"net"
	"os"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/vishvananda/netlink"
	"github.com/vishvananda/netlink/nl"
	"golang.org/x/sys/unix"
)

// IPv4Subnet — запись ipset. CIDR 0 означает одиночный адрес (/32).
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

// hostCIDR приводит маску одиночного адреса к 0. Адрес без маски ядро
// хранит в hash:net как /32 (/128) и при чтении возвращает маску явно,
// поэтому без приведения прочитанный хост не совпал бы с записанным и был
// бы удалён сверкой как лишний.
func hostCIDR(cidr, hostBits uint8) uint8 {
	if cidr == hostBits {
		return 0
	}
	return cidr
}

// IPv6Subnet — запись ipset. CIDR 0 означает одиночный адрес (/128).
type IPv6Subnet struct {
	Address [16]byte
	CIDR    uint8
}

func (subnet IPv6Subnet) String() string {
	if subnet.CIDR == 0 {
		return fmt.Sprintf("%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x", subnet.Address[0], subnet.Address[1], subnet.Address[2], subnet.Address[3], subnet.Address[4], subnet.Address[5], subnet.Address[6], subnet.Address[7], subnet.Address[8], subnet.Address[9], subnet.Address[10], subnet.Address[11], subnet.Address[12], subnet.Address[13], subnet.Address[14], subnet.Address[15])
	} else {
		return fmt.Sprintf("%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x/%d", subnet.Address[0], subnet.Address[1], subnet.Address[2], subnet.Address[3], subnet.Address[4], subnet.Address[5], subnet.Address[6], subnet.Address[7], subnet.Address[8], subnet.Address[9], subnet.Address[10], subnet.Address[11], subnet.Address[12], subnet.Address[13], subnet.Address[14], subnet.Address[15], subnet.CIDR)
	}
}

type IPSetTimeout *uint32

var zeroTimeout = IPSetTimeout(new(uint32))

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

	if timeout == nil {
		timeout = zeroTimeout
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

func (r *IPSet) AddIPv6Subnet(subnet IPv6Subnet, timeout IPSetTimeout) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil
	}

	if timeout == nil {
		timeout = zeroTimeout
	}

	err := netlink.IpsetAdd(r.ipsetName+"_6", &netlink.IPSetEntry{
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
	if err != nil && !errors.Is(err, nl.IPSetError(nl.IPSET_ERR_EXIST)) {
		return fmt.Errorf("failed to delete address: %w", err)
	}

	return nil
}

func (r *IPSet) DelIPv6Subnet(subnet IPv6Subnet) error {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil
	}

	err := netlink.IpsetDel(r.ipsetName+"_6", &netlink.IPSetEntry{
		IP:   subnet.Address[:],
		CIDR: subnet.CIDR,
	})
	if err != nil && !errors.Is(err, nl.IPSetError(nl.IPSET_ERR_EXIST)) {
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
		if len(entry.IP) != net.IPv4len {
			continue
		}
		subnet := IPv4Subnet{
			Address: [4]byte(entry.IP),
			CIDR:    hostCIDR(entry.CIDR, 32),
		}
		if entry.Timeout != nil && *entry.Timeout == 0 {
			addresses[subnet] = nil
		} else {
			addresses[subnet] = entry.Timeout
		}
	}

	return addresses, nil
}

func (r *IPSet) ListIPv6Subnets() (map[IPv6Subnet]IPSetTimeout, error) {
	r.locker.Lock()
	defer r.locker.Unlock()

	if !r.enabled.Load() {
		return nil, nil
	}

	addresses := make(map[IPv6Subnet]IPSetTimeout)

	list, err := netlink.IpsetList(r.ipsetName + "_6")
	if err != nil {
		return nil, err
	}
	for _, entry := range list.Entries {
		if len(entry.IP) != net.IPv6len {
			continue
		}
		subnet := IPv6Subnet{
			Address: [16]byte(entry.IP),
			CIDR:    hostCIDR(entry.CIDR, 128),
		}
		if entry.Timeout != nil && *entry.Timeout == 0 {
			addresses[subnet] = nil
		} else {
			addresses[subnet] = entry.Timeout
		}
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

// Name возвращает имя набора без суффикса семейства (_4, _6).
func (r *IPSet) Name() string {
	r.locker.Lock()
	defer r.locker.Unlock()
	return r.ipsetName
}

// Swap атомарно обменивает наборы r и other в ядре: правила iptables,
// ссылающиеся на имя r, сразу видят содержимое other, и наоборот. Имена в
// объектах тоже обмениваются, так что каждый объект продолжает указывать
// на своё содержимое. Семейства обмениваются по очереди; если второе не
// удалось, первое возвращается назад.
func (r *IPSet) Swap(other *IPSet) error {
	r.locker.Lock()
	defer r.locker.Unlock()
	other.locker.Lock()
	defer other.locker.Unlock()

	if !r.enabled.Load() || !other.enabled.Load() {
		return errors.New("failed to swap ipsets: both must be enabled")
	}
	if err := netlink.IpsetSwap(r.ipsetName+"_4", other.ipsetName+"_4"); err != nil {
		return fmt.Errorf("failed to swap ipv4 ipsets: %w", err)
	}
	if err := netlink.IpsetSwap(r.ipsetName+"_6", other.ipsetName+"_6"); err != nil {
		if backErr := netlink.IpsetSwap(r.ipsetName+"_4", other.ipsetName+"_4"); backErr != nil {
			err = errors.Join(err, fmt.Errorf("failed to swap ipv4 ipsets back: %w", backErr))
		}
		return fmt.Errorf("failed to swap ipv6 ipsets: %w", err)
	}
	r.ipsetName, other.ipsetName = other.ipsetName, r.ipsetName
	return nil
}

func (nh *Helper) IPSet(name string) *IPSet {
	return &IPSet{
		ipsetName: nh.IpsetPrefix + name,
	}
}

// ownIPSetName — имена наборов, которые создаёт приложение, без префикса:
// <id>-<слот>_<семейство>, а также старые форматы без слота и с "sub-".
var ownIPSetName = regexp.MustCompile(`^(sub-)?[0-9a-f]{8}(-[01])?_[46]$`)

// isOwnIPSet сообщает, что набор name создан приложением с префиксом prefix.
func isOwnIPSet(name, prefix string) bool {
	rest, ok := strings.CutPrefix(name, prefix)
	return ok && ownIPSetName.MatchString(rest)
}

// CleanIPSets удаляет ipset'ы приложения: с его префиксом и именем в его
// формате, чтобы не задеть чужие наборы с похожим префиксом. Вызывается при
// старте после CleanIPTables (ipset, на который ссылается правило iptables,
// удалить нельзя), чтобы не копились наборы, оставшиеся после аварийного
// завершения, — в том числе в старых форматах имён.
func (nh *Helper) CleanIPSets() error {
	if nh.IpsetPrefix == "" {
		return errors.New("ipset prefix is empty, refusing to destroy every ipset")
	}
	sets, err := netlink.IpsetListAll()
	if err != nil {
		return fmt.Errorf("failed to list ipsets: %w", err)
	}
	var errs []error
	for _, set := range sets {
		if !isOwnIPSet(set.SetName, nh.IpsetPrefix) {
			continue
		}
		if err := netlink.IpsetDestroy(set.SetName); err != nil && !os.IsNotExist(err) {
			errs = append(errs, fmt.Errorf("failed to destroy ipset %s: %w", set.SetName, err))
		}
	}
	return errors.Join(errs...)
}
