package app

import (
	"fmt"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
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
	switch event.Change {
	case 0x00000001:
		log.Trace().
			Str("interface", event.Link.Attrs().Name).
			Int("change", int(event.Change)).
			Msg("interface event")
		ifaceName := event.Link.Attrs().Name
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
	case 0xFFFFFFFF:
		switch event.Header.Type {
		case 16:
			log.Debug().
				Str("interface", event.Link.Attrs().Name).
				Int("type", int(event.Header.Type)).
				Msg("interface add")
		case 17:
			log.Debug().
				Str("interface", event.Link.Attrs().Name).
				Int("type", int(event.Header.Type)).
				Msg("interface del")
		}
	}
}
