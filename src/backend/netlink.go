package magitrickle

import (
	"fmt"
	"net"
	"slices"
	"sync"

	"magitrickle/constant"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
	"golang.org/x/sys/unix"
)

func subscribeLinkUpdates() (chan netlink.LinkUpdate, chan struct{}, error) {
	linkUpdateChannel := make(chan netlink.LinkUpdate)
	done := make(chan struct{})
	if err := netlink.LinkSubscribe(linkUpdateChannel, done); err != nil {
		return nil, nil, fmt.Errorf("failed to subscribe to link updates: %w", err)
	}
	return linkUpdateChannel, done, nil
}

func subscribeAddrUpdates() (chan netlink.AddrUpdate, chan struct{}, error) {
	addrUpdateChannel := make(chan netlink.AddrUpdate)
	done := make(chan struct{})
	if err := netlink.AddrSubscribe(addrUpdateChannel, done); err != nil {
		return nil, nil, fmt.Errorf("failed to subscribe to addr updates: %w", err)
	}
	return addrUpdateChannel, done, nil
}

// interfaceEvents копит события netlink по интерфейсам, пока их не
// применит applyInterfaceEvents. Новое событие заменяет прежнее того же
// вида для того же интерфейса: хуки всё равно перечитывают текущее
// состояние интерфейса, так что пачка событий обрабатывается один раз.
type interfaceEvents struct {
	mu    sync.Mutex
	links map[string]netlink.LinkUpdate
	addrs map[string]netlink.AddrUpdate
	// ready получает сигнал, когда есть необработанные события.
	ready chan struct{}
}

func newInterfaceEvents() *interfaceEvents {
	return &interfaceEvents{
		links: make(map[string]netlink.LinkUpdate),
		addrs: make(map[string]netlink.AddrUpdate),
		ready: make(chan struct{}, 1),
	}
}

func (q *interfaceEvents) putLink(ifaceName string, event netlink.LinkUpdate) {
	q.mu.Lock()
	q.links[ifaceName] = event
	q.mu.Unlock()
	q.signal()
}

func (q *interfaceEvents) putAddr(ifaceName string, event netlink.AddrUpdate) {
	q.mu.Lock()
	q.addrs[ifaceName] = event
	q.mu.Unlock()
	q.signal()
}

func (q *interfaceEvents) signal() {
	select {
	case q.ready <- struct{}{}:
	default:
	}
}

// take забирает накопленные события.
func (q *interfaceEvents) take() (map[string]netlink.LinkUpdate, map[string]netlink.AddrUpdate) {
	q.mu.Lock()
	defer q.mu.Unlock()
	links, addrs := q.links, q.addrs
	q.links, q.addrs = make(map[string]netlink.LinkUpdate), make(map[string]netlink.AddrUpdate)
	return links, addrs
}

// handleLink разбирает событие изменения состояния интерфейса и ставит
// поднявшийся интерфейс в очередь. Router здесь не вызывается.
func (a *App) handleLink(events *interfaceEvents, event netlink.LinkUpdate) {
	switch event.Header.Type {
	case unix.RTM_NEWLINK:
		linkAttrs := event.Link.Attrs()
		if linkAttrs.Flags&net.FlagUp == 0 {
			break
		}

		ifaceName := linkAttrs.Name
		if !slices.Contains(constant.IgnoredInterfaces, ifaceName) {
			log.Debug().
				Str("interface", ifaceName).
				Int("type", int(event.Header.Type)).
				Msg("interface up")
		}
		events.putLink(ifaceName, event)

	case unix.RTM_DELLINK:
		log.Debug().
			Str("interface", event.Link.Attrs().Name).
			Int("type", int(event.Header.Type)).
			Msg("interface del")
	}
}

// handleAddr разбирает событие изменения IP-адресов интерфейса и ставит
// интерфейс в очередь. Router здесь не вызывается.
func (a *App) handleAddr(events *interfaceEvents, event netlink.AddrUpdate) {
	if !event.NewAddr {
		return
	}

	iface, err := netlink.LinkByIndex(event.LinkIndex)
	if err != nil {
		log.Error().Err(err).Int("linkIndex", event.LinkIndex).Msg("failed to get interface for addr update")
		return
	}

	ifaceName := iface.Attrs().Name
	if !slices.Contains(constant.IgnoredInterfaces, ifaceName) {
		log.Debug().
			Str("interface", ifaceName).
			Str("addr", event.LinkAddress.String()).
			Msg("interface address changed")
	}
	events.putAddr(ifaceName, event)
}

// applyInterfaceEvents передаёт накопленные события маршрутам.
func (a *App) applyInterfaceEvents(events *interfaceEvents) {
	links, addrs := events.take()
	for ifaceName, event := range links {
		if err := a.router.LinkUpHook(event); err != nil {
			log.Error().
				Err(err).
				Str("interface", ifaceName).
				Msg("error while handling interface up")
		}
	}
	for ifaceName, event := range addrs {
		if err := a.router.AddrChangeHook(ifaceName, event); err != nil {
			log.Error().
				Err(err).
				Str("interface", ifaceName).
				Msg("error while handling interface addr change")
		}
	}
}
