package magitrickle

import (
	"errors"
	"fmt"
	"net"
	"slices"
	"sync/atomic"

	"magitrickle/app"
	"magitrickle/constant"
	"magitrickle/models"
	"magitrickle/utils/dnsMITMProxy"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"

	"github.com/rs/zerolog/log"
)

var (
	ErrAlreadyRunning           = errors.New("already running")
	ErrGroupIDConflict          = errors.New("group id conflict")
	ErrRuleIDConflict           = errors.New("rule id conflict")
	ErrConfigUnsupportedVersion = errors.New("config unsupported version")
)

// App – основная структура ядра приложения
type App struct {
	enabled atomic.Bool

	config models.AppConfig

	dnsMITM      *dnsMITMProxy.DNSMITMProxy
	nfHelper     *netfilterTools.Helper
	recordsCache *recordsCache.Records
	groups       atomic.Pointer[[]*Group]
	dnsOverrider *netfilterTools.PortRemap
}

// New создаёт новый экземпляр App
func New() *App {
	a := &App{
		config: constant.DefaultAppConfig,
	}
	emptyGroups := make([]*Group, 0)
	a.groups.Store(&emptyGroups)
	if err := a.LoadConfig(); err != nil {
		log.Error().Err(err).Msg("failed to load config file")
	}
	return a
}

// Config возвращает конфигурацию
func (a *App) Config() models.AppConfig {
	return a.config
}

// Groups возвращает список групп
func (a *App) Groups() []app.Group {
	gs := *a.groups.Load()
	groups := make([]app.Group, len(gs))
	for i, g := range gs {
		groups[i] = g
	}
	return groups
}

// ClearGroups отключает все группы и очищает список
func (a *App) ClearGroups() {
	for _, g := range *a.groups.Load() {
		_ = g.Disable()
	}
	emptyGroups := make([]*Group, 0)
	a.groups.Store(&emptyGroups)
}

// AddGroup добавляет новую группу
func (a *App) AddGroup(groupModel *models.Group) error {
	groups := *a.groups.Load()
	for _, group := range groups {
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
	newGroups := make([]*Group, len(groups)+1)
	copy(newGroups, groups)
	newGroups[len(groups)] = grp
	a.groups.Store(&newGroups)

	log.Info().
		Str("id", grp.ID.String()).
		Str("name", grp.Name).
		Msg("added group")

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
	groups := *a.groups.Load()
	newGroups := make([]*Group, 0, len(groups)-1)
	newGroups = append(newGroups, groups[:idx]...)
	newGroups = append(newGroups, groups[idx+1:]...)
	a.groups.Store(&newGroups)
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
		if iface.Flags&net.FlagPointToPoint == 0 || slices.Contains(constant.IgnoredInterfaces, iface.Name) {
			continue
		}
		filteredInterfaces = append(filteredInterfaces, iface)
	}
	return filteredInterfaces, nil
}

// DnsOverrider возвращает dnsOverrider
func (a *App) DnsOverrider() *netfilterTools.PortRemap {
	return a.dnsOverrider
}
