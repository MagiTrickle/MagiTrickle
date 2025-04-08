package app

import (
	"context"
	"fmt"
	"os"
	"runtime/debug"

	netfilterHelper "magitrickle/netfilter-helper"

	"github.com/rs/zerolog"
	"github.com/vishvananda/netlink"
	"github.com/vishvananda/netlink/nl"
)

// Start запускает приложение (ядро)
func (a *App) Start(ctx context.Context) error {
	if !a.enabled.CompareAndSwap(false, true) {
		return ErrAlreadyRunning
	}
	defer a.enabled.Store(false)

	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "panic: %s\n", debug.Stack())
		}
	}()

	a.setupLogging()
	a.initDNSMITM()

	nfh, err := a.createNetfilterHelper()
	if err != nil {
		return fmt.Errorf("netfilter helper init fail: %w", err)
	}
	a.nfHelper = nfh

	if err := a.nfHelper.CleanIPTables(); err != nil {
		return fmt.Errorf("failed to clear iptables: %w", err)
	}

	newCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	errChan := make(chan error)

	a.startDNSListeners(newCtx, errChan)

	for _, group := range a.groups {
		if err := group.Enable(); err != nil {
			return fmt.Errorf("failed to enable group: %w", err)
		}
	}
	defer func() {
		for _, group := range a.groups {
			_ = group.Disable()
		}
	}()

	linkUpdateChannel, linkUpdateDone, err := subscribeLinkUpdates()
	if err != nil {
		return err
	}
	defer close(linkUpdateDone)

	for {
		select {
		case event := <-linkUpdateChannel:
			a.handleLink(event)
		case err := <-errChan:
			return err
		case <-ctx.Done():
			return nil
		}
	}
}

func (a *App) setupLogging() {
	switch a.config.LogLevel {
	case "trace":
		zerolog.SetGlobalLevel(zerolog.TraceLevel)
	case "debug":
		zerolog.SetGlobalLevel(zerolog.DebugLevel)
	case "info":
		zerolog.SetGlobalLevel(zerolog.InfoLevel)
	case "warn":
		zerolog.SetGlobalLevel(zerolog.WarnLevel)
	case "error":
		zerolog.SetGlobalLevel(zerolog.ErrorLevel)
	case "fatal":
		zerolog.SetGlobalLevel(zerolog.FatalLevel)
	case "panic":
		zerolog.SetGlobalLevel(zerolog.PanicLevel)
	case "nolevel":
		zerolog.SetGlobalLevel(zerolog.NoLevel)
	case "disabled":
		zerolog.SetGlobalLevel(zerolog.Disabled)
	default:
		zerolog.SetGlobalLevel(zerolog.InfoLevel)
	}
}

func (a *App) createNetfilterHelper() (*netfilterHelper.NetfilterHelper, error) {
	return netfilterHelper.New(a.config.Netfilter.IPTables.ChainPrefix, a.config.Netfilter.IPSet.TablePrefix, a.config.Netfilter.DisableIPv4, a.config.Netfilter.DisableIPv6)
}

func (a *App) getInterfaceAddresses() ([]netlink.Addr, error) {
	var addrList []netlink.Addr
	for _, linkName := range a.config.Link {
		link, err := netlink.LinkByName(linkName)
		if err != nil {
			return nil, fmt.Errorf("failed to find link %s: %w", linkName, err)
		}
		linkAddrList, err := netlink.AddrList(link, nl.FAMILY_ALL)
		if err != nil {
			return nil, fmt.Errorf("failed to list address of interface %s: %w", linkName, err)
		}
		addrList = append(addrList, linkAddrList...)
	}
	return addrList, nil
}
