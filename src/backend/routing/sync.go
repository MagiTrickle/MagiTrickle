package routing

import (
	"errors"
	"fmt"
	"net"
	"net/netip"
	"time"

	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"

	"github.com/rs/zerolog/log"
)

// fill наполняет новые маршруты тем, что следует из правил: подсети из
// subnet-правил добавляются бессрочно, адреса доменов из кэша — на
// оставшееся время жизни записи. Маршруты новые — их ipset'ы пусты и трафик
// через них ещё не идёт, поэтому ничего удалять не нужно. Возвращает ошибки
// по ключу набора: такой маршрут наполнен не полностью.
//
// Одновременно маршруты наполняются и DNS-ответами. Всё, что DNS добавил во
// время fill, записывается в журнал и после наполнения добавляется заново:
// иначе fill мог бы перезаписать свежий TTL устаревшим из кэша.
//
// Кэш DNS обходится один раз на все маршруты.
func (t *Table) fill(entries []entry) map[string]error {
	wants := make([]subnets, len(entries))
	for i, e := range entries {
		t.startJournal(e.route)
		wants[i] = newSubnets()
		for _, prefix := range e.ruleSet.Subnets {
			wants[i].addPrefix(prefix)
		}
	}

	now := time.Now()
	for _, domain := range t.records.ListKnownDomains() {
		var addresses []*recordsCache.Address
		loaded := false
		for i, e := range entries {
			if !e.ruleSet.Domains.Match(domain) {
				continue
			}
			if !loaded {
				addresses, loaded = t.records.GetAddresses(domain), true
			}
			for _, address := range addresses {
				if ttl := address.Deadline.Sub(now).Seconds(); ttl > 0 {
					wants[i].addAddress(address.Address, uint32(ttl))
				}
			}
		}
	}

	failed := make(map[string]error)
	for i, e := range entries {
		err := errors.Join(
			write(wants[i].v4, e.route.AddIPv4Subnet),
			write(wants[i].v6, e.route.AddIPv6Subnet),
			t.replayJournal(e.route),
		)
		if err != nil {
			failed[e.ruleSet.Key()] = err
		}
	}
	return failed
}

// replayJournal проигрывает журнал маршрута, пока он не опустеет: DNS-ответ,
// пришедший во время проигрывания, тоже попадает в журнал и проигрывается
// последним, поэтому более старая запись не может перекрыть более свежую.
func (t *Table) replayJournal(route Route) error {
	var errs []error
	for {
		addresses, done := t.drainJournal(route)
		if done {
			return errors.Join(errs...)
		}
		for _, address := range addresses {
			if err := addAddress(route, address); err != nil {
				errs = append(errs, fmt.Errorf("failed to re-add %s: %w", address.IP, err))
			}
		}
	}
}

func (t *Table) startJournal(route Route) {
	t.journalMu.Lock()
	defer t.journalMu.Unlock()
	if t.journals == nil {
		t.journals = make(map[Route][]Address)
	}
	t.journals[route] = []Address{}
}

// drainJournal забирает накопленные адреса. Если журнал пуст, удаляет его:
// после этого DNS пишет в маршрут напрямую, и его записи идут позже всех
// записей fill.
func (t *Table) drainJournal(route Route) (addresses []Address, done bool) {
	t.journalMu.Lock()
	defer t.journalMu.Unlock()
	addresses = t.journals[route]
	if len(addresses) == 0 {
		delete(t.journals, route)
		return nil, true
	}
	t.journals[route] = []Address{}
	return addresses, false
}

// journal записывает адрес в журнал маршрута, если тот сейчас наполняется.
// Вызывается до записи в ipset: иначе запись могла бы пройти во время
// сверки, а в журнал попасть уже после её окончания.
func (t *Table) journal(route Route, address Address) {
	t.journalMu.Lock()
	defer t.journalMu.Unlock()
	if addresses, ok := t.journals[route]; ok {
		t.journals[route] = append(addresses, address)
	}
}

// subnets — желаемое содержимое ipset; nil-таймаут означает бессрочную запись.
type subnets struct {
	v4 map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout
	v6 map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout
}

func newSubnets() subnets {
	return subnets{
		v4: make(map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout),
		v6: make(map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout),
	}
}

// addPrefix добавляет подсеть бессрочно. Одиночный адрес записывается как
// хост (CIDR 0) — так же, как его пишет DNS: иначе одна и та же запись
// попала бы в want дважды.
func (s subnets) addPrefix(prefix netip.Prefix) {
	addr, bits := prefix.Addr(), uint8(prefix.Bits())
	if prefix.IsSingleIP() {
		bits = 0
	}
	if addr.Is4() {
		if prefix.Bits() == 0 {
			// TODO: Fix (remove dirty hack) after resolving https://github.com/vishvananda/netlink/issues/1091
			s.v4[netfilterTools.IPv4Subnet{Address: [4]byte{0x00}, CIDR: 1}] = nil
			s.v4[netfilterTools.IPv4Subnet{Address: [4]byte{0x80}, CIDR: 1}] = nil
			return
		}
		s.v4[netfilterTools.IPv4Subnet{Address: addr.As4(), CIDR: bits}] = nil
		return
	}
	if prefix.Bits() == 0 {
		s.v6[netfilterTools.IPv6Subnet{Address: [16]byte{0x00}, CIDR: 1}] = nil
		s.v6[netfilterTools.IPv6Subnet{Address: [16]byte{0x80}, CIDR: 1}] = nil
		return
	}
	s.v6[netfilterTools.IPv6Subnet{Address: addr.As16(), CIDR: bits}] = nil
}

// addAddress добавляет адрес хоста, оставляя наибольшее время жизни.
// Бессрочная запись от subnet-правила остаётся бессрочной.
func (s subnets) addAddress(ip net.IP, ttl uint32) {
	switch len(ip) {
	case net.IPv4len:
		putLonger(s.v4, netfilterTools.IPv4Subnet{Address: [4]byte(ip)}, ttl)
	case net.IPv6len:
		putLonger(s.v6, netfilterTools.IPv6Subnet{Address: [16]byte(ip)}, ttl)
	}
}

func putLonger[S comparable](m map[S]netfilterTools.IPSetTimeout, subnet S, ttl uint32) {
	if current, ok := m[subnet]; ok && (current == nil || *current >= ttl) {
		return
	}
	m[subnet] = &ttl
}

// write добавляет записи в ipset. Не останавливается на первой ошибке;
// возвращает первую и число неудач.
func write[S interface {
	comparable
	fmt.Stringer
}](want map[S]netfilterTools.IPSetTimeout, add func(S, netfilterTools.IPSetTimeout) error) error {
	var failed int
	var first error
	for subnet, ttl := range want {
		if err := add(subnet, ttl); err != nil {
			log.Error().Err(err).Str("subnet", subnet.String()).Msg("failed to add subnet")
			if failed == 0 {
				first = fmt.Errorf("failed to add %s: %w", subnet, err)
			}
			failed++
		}
	}
	if failed > 1 {
		return fmt.Errorf("%w (and %d more)", first, failed-1)
	}
	return first
}
