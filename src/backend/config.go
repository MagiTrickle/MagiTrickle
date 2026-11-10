package magitrickle

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"magitrickle/constant"
	"magitrickle/models"
	subscriptionsPkg "magitrickle/subscriptions"
	"magitrickle/utils/intID"

	"github.com/dlclark/regexp2"
	"github.com/rs/zerolog/log"
	"go.yaml.in/yaml/v2"
)

var colorRegExp = regexp2.MustCompile(`^#[0-9a-f]{6}$`, regexp2.IgnoreCase)

const cfgFolderLocation = constant.AppStateDir
const cfgFileLocation = cfgFolderLocation + "/config.yaml"

// LoadConfig загружает конфиг целиком или не меняет ничего: настройки,
// группы и подписки собираются и проверяются заранее, а подменяются вместе.
//
// Пока конфиг ни разу не загрузился, SaveConfig отказывается писать файл:
// иначе после неудачной загрузки при старте первое же сохранение затёрло
// бы его пустым состоянием. Если же не удалось перечитать файл позже, в
// памяти остаётся загруженное ранее рабочее состояние, и сохранять его
// можно — правки из UI не должны молча теряться. Start без загруженного
// конфига не запускается.
func (a *App) LoadConfig() error {
	err := a.loadConfig()
	if err == nil {
		a.configLoaded.Store(true)
	} else if !a.configLoaded.Load() {
		a.configErr.Store(&err)
	}
	return err
}

func (a *App) loadConfig() error {
	return a.loadConfigFile(cfgFileLocation)
}

func (a *App) loadConfigFile(path string) error {
	cfgFile, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return fmt.Errorf("failed to read config file: %w", err)
	}
	// Decode into a copy and take it over only when the whole file is fine: a type error in the middle of the file
	// must not leave a reload half applied.
	next := *a.cfg()
	cfg := models.Config{App: &next}
	err = yaml.Unmarshal(cfgFile, &cfg)
	if err != nil {
		return fmt.Errorf("failed to unmarshal config file: %w", err)
	}

	if !strings.HasPrefix(cfg.ConfigVersion, "0.") {
		return ErrConfigUnsupportedVersion
	}

	// TODO: Remove this align to milliseconds/seconds before release 1.0.0
	if next.DNSProxy.Timeout < time.Millisecond {
		next.DNSProxy.Timeout *= time.Millisecond
	}
	if next.Netfilter.IPSet.AdditionalTTL < time.Second {
		next.Netfilter.IPSet.AdditionalTTL *= time.Second
	}

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	var groups []*models.Group
	if cfg.Groups != nil {
		groups = *cfg.Groups
	}
	var subscriptions []*models.Subscription
	if cfg.Subscriptions != nil {
		subscriptions = *cfg.Subscriptions
	}
	groups, subscriptions = repairConfig(groups, subscriptions)

	for _, group := range groups {
		if match, _ := colorRegExp.MatchString(group.Color); !match {
			group.Color = "#ffffff"
		} else {
			group.Color = strings.ToLower(group.Color)
		}
	}

	if err := validateAppConfig(&next); err != nil {
		return err
	}
	if err := validateSubscriptions(subscriptions, nil); err != nil {
		return err
	}
	if err := validateGroups(groups, subscriptions); err != nil {
		return err
	}

	previousGroups, previousSubscriptions := a.groups, a.subscriptions
	a.groups, a.subscriptions = groups, subscriptions
	err = a.commitLocked(func() {
		a.groups, a.subscriptions = previousGroups, previousSubscriptions
	})
	if err != nil {
		return err
	}
	a.config.Store(&next)
	return nil
}

// Имена в ядре ограничены: ipset — 31 символом, цепочка iptables — 28.
// Имя маршрута — префикс, ID (8 символов) и слот ("-0"), у ipset ещё
// семейство ("_4").
const (
	maxChainPrefixLen = 28 - 8 - 2
	maxIPSetPrefixLen = 31 - 8 - 2 - 2
)

// validateAppConfig проверяет настройки, от которых зависят имена в ядре.
// Пустой префикс опасен: по префиксу при старте удаляются оставшиеся с
// прошлого запуска цепочки и ipset'ы, и пустой совпал бы с чужими.
func validateAppConfig(c *models.AppConfig) error {
	chainPrefix, ipsetPrefix := c.Netfilter.IPTables.ChainPrefix, c.Netfilter.IPSet.TablePrefix
	// Сообщение указывает ключ в config.yaml: после обновления это первое,
	// что увидит пользователь с прежде допустимым длинным префиксом.
	const chainKey, ipsetKey = "app.netfilter.iptables.chainPrefix", "app.netfilter.ipset.tablePrefix"
	switch {
	case chainPrefix == "":
		return fmt.Errorf("%s must not be empty", chainKey)
	case len(chainPrefix) > maxChainPrefixLen:
		return fmt.Errorf("%s %q is longer than %d characters, shorten it", chainKey, chainPrefix, maxChainPrefixLen)
	case ipsetPrefix == "":
		return fmt.Errorf("%s must not be empty", ipsetKey)
	case len(ipsetPrefix) > maxIPSetPrefixLen:
		return fmt.Errorf("%s %q is longer than %d characters, shorten it", ipsetKey, ipsetPrefix, maxIPSetPrefixLen)
	}
	return nil
}

// repairConfig чинит в загруженном конфиге то, из-за чего не стоит
// отказываться от него целиком (обычно это следы правки руками): пустые
// записи и пустые или повторяющиеся ID правил. ID групп и подписок не
// чинятся — по ним именуются маршруты и адресуется API, так что их
// конфликт остаётся ошибкой загрузки.
func repairConfig(groups []*models.Group, subscriptions []*models.Subscription) ([]*models.Group, []*models.Subscription) {
	groups = slices.DeleteFunc(groups, func(group *models.Group) bool { return group == nil })
	for _, group := range groups {
		group.Rules = slices.DeleteFunc(group.Rules, func(rule *models.Rule) bool { return rule == nil })
		var fixed int
		group.Rules, fixed = intID.EnsureUnique(group.Rules,
			func(rule *models.Rule) intID.ID { return rule.ID },
			(*models.Rule).WithID)
		if fixed > 0 {
			log.Warn().Str("group", group.ID.String()).Int("rules", fixed).Msg("assigned new IDs to group rules with missing or duplicate IDs")
		}
	}

	subscriptions = slices.DeleteFunc(subscriptions, func(sub *models.Subscription) bool { return sub == nil })
	for _, sub := range subscriptions {
		sub.Rules = slices.DeleteFunc(sub.Rules, func(rule *models.SubscriptionRule) bool { return rule == nil })
		var fixed int
		if sub.Rules, fixed = subscriptionsPkg.EnsureUniqueRuleIDs(sub.Rules); fixed > 0 {
			log.Warn().Str("subscription", sub.ID.String()).Int("rules", fixed).Msg("assigned new IDs to subscription rules with missing or duplicate IDs")
		}
	}
	return groups, subscriptions
}

func (a *App) SaveConfig() error {
	if !a.configLoaded.Load() {
		return fmt.Errorf("%w; fix it and reload before saving, so it is not overwritten", ErrConfigNotLoaded)
	}

	// Saves are serialized, so the last one to finish also holds the newest state: SaveConfig is called from
	// the API handlers and from the subscription updater at the same time.
	a.configSaveMu.Lock()
	defer a.configSaveMu.Unlock()

	// Настройки читаются под той же блокировкой, под которой LoadConfig
	// подменяет их вместе с группами, — иначе в файл попали бы старые
	// настройки с новыми группами.
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()

	cfg := models.Config{
		ConfigVersion: constant.Version,
		App:           a.cfg(),
		Groups:        &a.groups,
		Subscriptions: &a.subscriptions,
	}
	out, err := yaml.Marshal(cfg)
	if err != nil {
		return fmt.Errorf("failed to marshal config file: %w", err)
	}
	if err := os.MkdirAll(cfgFolderLocation, os.ModePerm); err != nil {
		return fmt.Errorf("failed to create config folder: %w", err)
	}
	if err := writeFileAtomic(cfgFileLocation, out, 0600); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}
	return nil
}

// writeFileAtomic replaces path with data so that a crash or a power cut leaves either the old or the new file, not
// a truncated one: the data is flushed to disk before the rename, and the directory after it.
//
// Every call writes its own temporary file, so concurrent calls do not trip over each other.
func writeFileAtomic(path string, data []byte, perm os.FileMode) error {
	f, err := os.CreateTemp(filepath.Dir(path), filepath.Base(path)+".*.tmp")
	if err != nil {
		return fmt.Errorf("failed to create temporary file: %w", err)
	}
	tmp := f.Name()
	err = f.Chmod(perm)
	if err == nil {
		_, err = f.Write(data)
	}
	if err == nil {
		err = f.Sync()
	}
	if closeErr := f.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("failed to write temporary file: %w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("failed to replace file: %w", err)
	}
	// Best effort: not every filesystem lets a directory be synced.
	if dir, err := os.Open(filepath.Dir(path)); err == nil {
		_ = dir.Sync()
		_ = dir.Close()
	}
	return nil
}
