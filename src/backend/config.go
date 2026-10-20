package magitrickle

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"magitrickle/constant"
	"magitrickle/models"

	"github.com/dlclark/regexp2"
	"go.yaml.in/yaml/v2"
)

var colorRegExp = regexp2.MustCompile(`^#[0-9a-f]{6}$`, regexp2.IgnoreCase)

const cfgFolderLocation = constant.AppStateDir
const cfgFileLocation = cfgFolderLocation + "/config.yaml"

func (a *App) LoadConfig() error {
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
	settings := a.config
	cfg := models.Config{App: &settings}
	err = yaml.Unmarshal(cfgFile, &cfg)
	if err != nil {
		return fmt.Errorf("failed to unmarshal config file: %w", err)
	}

	if !strings.HasPrefix(cfg.ConfigVersion, "0.") {
		return ErrConfigUnsupportedVersion
	}

	// TODO: Remove this align to milliseconds/seconds before release 1.0.0
	if settings.DNSProxy.Timeout < time.Millisecond {
		settings.DNSProxy.Timeout *= time.Millisecond
	}
	if settings.Netfilter.IPSet.AdditionalTTL < time.Second {
		settings.Netfilter.IPSet.AdditionalTTL *= time.Second
	}

	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	a.config = settings

	if cfg.Groups != nil {
		// отключаем старые группы и очищаем срез
		for _, group := range a.userRuleSets {
			_ = group.Disable()
		}
		a.userRuleSets = a.userRuleSets[:0]

		for _, group := range *cfg.Groups {
			if match, _ := colorRegExp.MatchString(group.Color); !match {
				group.Color = "#ffffff"
			} else {
				group.Color = strings.ToLower(group.Color)
			}
			if err := a.addGroupLocked(group); err != nil {
				return err
			}
		}
	}

	if cfg.Subscriptions != nil {
		a.subscriptions = append(a.subscriptions[:0], *cfg.Subscriptions...)
	} else {
		a.subscriptions = a.subscriptions[:0]
	}

	return a.syncSubscriptionRuleSetsLocked()
}

func (a *App) SaveConfig() error {
	// Saves are serialized, so the last one to finish also holds the newest state: SaveConfig is called from
	// the API handlers and from the subscription updater at the same time.
	a.configSaveMu.Lock()
	defer a.configSaveMu.Unlock()

	a.stateMu.RLock()
	defer a.stateMu.RUnlock()

	groups := make([]*models.Group, 0, len(a.userRuleSets))
	for _, rs := range a.userRuleSets {
		if gm := rs.Model(); gm != nil {
			groups = append(groups, gm)
		}
	}

	cfg := models.Config{
		ConfigVersion: constant.Version,
		App:           &a.config,
		Groups:        &groups,
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
