package app

import (
	"errors"
	"fmt"
	"net"
	"sync/atomic"

	dnsMitmProxy "magitrickle/dns-mitm-proxy"
	"magitrickle/models"
	netfilterHelper "magitrickle/netfilter-helper"
	"magitrickle/records"

	"github.com/rs/zerolog/log"
)

var (
	ErrAlreadyRunning           = errors.New("already running")
	ErrGroupIDConflict          = errors.New("group id conflict")
	ErrRuleIDConflict           = errors.New("rule id conflict")
	ErrConfigUnsupportedVersion = errors.New("config unsupported version")
)

var defaultAppConfig = models.App{
	DNSProxy: models.DNSProxy{
		Host:            models.DNSProxyServer{Address: "[::]", Port: 3553},
		Upstream:        models.DNSProxyServer{Address: "127.0.0.1", Port: 53},
		DisableRemap53:  false,
		DisableFakePTR:  false,
		DisableDropAAAA: false,
	},
	HTTPWeb: models.HTTPWeb{
		Enabled: true,
		Host: models.HTTPWebServer{
			Address: "[::]",
			Port:    8080,
		},
		Skin: "default",
	},
	Netfilter: models.Netfilter{
		IPTables: models.IPTables{
			ChainPrefix: "MT_",
		},
		IPSet: models.IPSet{
			TablePrefix:   "mt_",
			AdditionalTTL: 3600,
		},
		DisableIPv4: false,
		DisableIPv6: false,
	},
	Link:              []string{"br0"},
	ShowAllInterfaces: false,
	LogLevel:          "info",
}

// App – основная структура ядра приложения
type App struct {
	config   models.App
	dnsMITM  *dnsMitmProxy.DNSMITMProxy
	nfHelper *netfilterHelper.NetfilterHelper
	records  *records.Records
	groups   []*Group
	// TODO: доделать
	enabled      atomic.Bool
	dnsOverrider *netfilterHelper.PortRemap
}

// New создаёт новый экземпляр App
func New() *App {
	a := &App{
		config: defaultAppConfig,
	}
	if err := a.LoadConfig(); err != nil {
		log.Error().Err(err).Msg("failed to load config file")
	}
	return a
}

// Config возвращает конфигурацию
func (a *App) Config() models.App {
	return a.config
}

// Groups возвращает список групп
func (a *App) Groups() []*Group {
	return a.groups
}

// ClearGroups отключает все группы и очищает список
func (a *App) ClearGroups() {
	for _, g := range a.groups {
		_ = g.Disable()
	}
	a.groups = a.groups[:0]
}

// AddGroup добавляет новую группу
func (a *App) AddGroup(groupModel *models.Group) error {
	for _, group := range a.groups {
		if groupModel.ID == group.ID {
			return ErrGroupIDConflict
		}
	}
	// Проверка уникальности rule.ID внутри группы.
	dup := make(map[[4]byte]struct{})
	for _, rule := range groupModel.Rules {
		if _, exists := dup[rule.ID]; exists {
			return ErrRuleIDConflict
		}
		dup[rule.ID] = struct{}{}
	}

	grp, err := NewGroup(groupModel, a)
	if err != nil {
		return fmt.Errorf("failed to create group: %w", err)
	}
	a.groups = append(a.groups, grp)

	log.Debug().Str("id", grp.ID.String()).Str("name", grp.Name).Msg("added group")

	// если приложение уже запущено – включаем группу и выполняем синхронизацию
	if a.enabled.Load() {
		if err = grp.Enable(); err != nil {
			return fmt.Errorf("failed to enable group: %w", err)
		}
		if err = grp.Sync(); err != nil {
			return fmt.Errorf("failed to sync group: %w", err)
		}
	}
	return nil
}

// RemoveGroupByIndex удаляет группу по индексу
func (a *App) RemoveGroupByIndex(idx int) {
	a.groups = append(a.groups[:idx], a.groups[idx+1:]...)
}

// ListInterfaces возвращает список сетевых интерфейсов, удовлетворяющих заданным критериям
func (a *App) ListInterfaces() ([]net.Interface, error) {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("failed to get interfaces: %w", err)
	}

	if a.config.ShowAllInterfaces {
		return interfaces, nil
	}

	var filteredInterfaces []net.Interface
	for _, iface := range interfaces {
		if iface.Flags&net.FlagPointToPoint == 0 {
			continue
		}
		filteredInterfaces = append(filteredInterfaces, iface)
	}
	return filteredInterfaces, nil
}

// DnsOverrider возвращает dnsOverrider
func (a *App) DnsOverrider() *netfilterHelper.PortRemap {
	return a.dnsOverrider
}
