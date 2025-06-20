package app

import (
	"fmt"
	"slices"

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

// handleLink обрабатывает события изменения состояния сетевых интерфейсов
func (a *App) handleLink(event netlink.LinkUpdate) {
	switch event.Header.Type {
	case unix.RTM_NEWLINK:
		ifaceName := event.Link.Attrs().Name
		if !slices.Contains(constant.IgnoredInterfaces, ifaceName) {
			log.Debug().
				Str("interface", ifaceName).
				Int("type", int(event.Header.Type)).
				Msg("interface add")
		}
		for _, group := range a.groups {
			if group.Interface != ifaceName {
				continue
			}
			if err := group.LinkUpdateHook(event); err != nil {
				log.Error().
					Str("group", group.ID.String()).
					Err(err).
					Msg("error while handling interface up")
			}
		}
	case unix.RTM_DELLINK:
		log.Debug().
			Str("interface", event.Link.Attrs().Name).
			Int("type", int(event.Header.Type)).
			Msg("interface del")
	}
}
