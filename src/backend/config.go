package magitrickle

import (
	"errors"
	"fmt"
	"os"
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
	cfgFile, err := os.ReadFile(cfgFileLocation)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return fmt.Errorf("failed to read config file: %w", err)
	}
	cfg := models.Config{App: &a.config}
	err = yaml.Unmarshal(cfgFile, &cfg)
	if err != nil {
		return fmt.Errorf("failed to unmarshal config file: %w", err)
	}

	if !strings.HasPrefix(cfg.ConfigVersion, "0.") {
		return ErrConfigUnsupportedVersion
	}

	// TODO: Remove this align to milliseconds/seconds before release 1.0.0
	if a.config.DNSProxy.Timeout < time.Millisecond {
		a.config.DNSProxy.Timeout *= time.Millisecond
	}
	if a.config.Netfilter.IPSet.AdditionalTTL < time.Second {
		a.config.Netfilter.IPSet.AdditionalTTL *= time.Second
	}

	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

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
	tmpFileLocation := cfgFileLocation + ".tmp"
	if err := os.WriteFile(tmpFileLocation, out, 0600); err != nil {
		return fmt.Errorf("failed to write temporary config file: %w", err)
	}
	if err := os.Rename(tmpFileLocation, cfgFileLocation); err != nil {
		_ = os.Remove(tmpFileLocation)
		return fmt.Errorf("failed to replace config file: %w", err)
	}
	return nil
}
