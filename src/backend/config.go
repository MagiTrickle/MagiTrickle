package magitrickle

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"magitrickle/config"
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
	cfg := config.Config{}
	err = yaml.Unmarshal(cfgFile, &cfg)
	if err != nil {
		return fmt.Errorf("failed to unmarshal config file: %w", err)
	}
	err = a.ImportConfig(cfg)
	if err != nil {
		return fmt.Errorf("failed to import config file: %w", err)
	}
	return nil
}

func (a *App) SaveConfig() error {
	out, err := yaml.Marshal(a.ExportConfig())
	if err != nil {
		return fmt.Errorf("failed to marshal config file: %w", err)
	}
	if err := os.MkdirAll(cfgFolderLocation, os.ModePerm); err != nil {
		return fmt.Errorf("failed to create config folder: %w", err)
	}
	if err := os.WriteFile(cfgFileLocation, out, 0600); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}
	return nil
}

func applyIfSet[T any](dst *T, src *T) {
	if src != nil {
		*dst = *src
	}
}

func (a *App) ImportConfig(cfg config.Config) error {
	if !strings.HasPrefix(cfg.ConfigVersion, "0.") {
		return ErrConfigUnsupportedVersion
	}

	if cfg.App != nil {
		if cfg.App.HTTPWeb != nil {
			applyIfSet(&a.config.HTTPWeb.Enabled, cfg.App.HTTPWeb.Enabled)
			applyIfSet(&a.config.HTTPWeb.Skin, cfg.App.HTTPWeb.Skin)
			if cfg.App.HTTPWeb.Host != nil {
				applyIfSet(&a.config.HTTPWeb.Host.Address, cfg.App.HTTPWeb.Host.Address)
				applyIfSet(&a.config.HTTPWeb.Host.Port, cfg.App.HTTPWeb.Host.Port)
			}
			if cfg.App.HTTPWeb.Auth != nil {
				applyIfSet(&a.config.HTTPWeb.Auth.Enabled, cfg.App.HTTPWeb.Auth.Enabled)
			}
		}

		if cfg.App.DNSProxy != nil {
			if cfg.App.DNSProxy.Upstream != nil {
				applyIfSet(&a.config.DNSProxy.Upstream.Address, cfg.App.DNSProxy.Upstream.Address)
				applyIfSet(&a.config.DNSProxy.Upstream.Port, cfg.App.DNSProxy.Upstream.Port)
			}
			if cfg.App.DNSProxy.Host != nil {
				applyIfSet(&a.config.DNSProxy.Host.Address, cfg.App.DNSProxy.Host.Address)
				applyIfSet(&a.config.DNSProxy.Host.Port, cfg.App.DNSProxy.Host.Port)
			}
			applyIfSet(&a.config.DNSProxy.DisableRemap53, cfg.App.DNSProxy.DisableRemap53)
			applyIfSet(&a.config.DNSProxy.DisableFakePTR, cfg.App.DNSProxy.DisableFakePTR)
			applyIfSet(&a.config.DNSProxy.DisableDropAAAA, cfg.App.DNSProxy.DisableDropAAAA)
			applyIfSet(&a.config.DNSProxy.MaxIdleConns, cfg.App.DNSProxy.MaxIdleConns)
			applyIfSet(&a.config.DNSProxy.MaxConcurrent, cfg.App.DNSProxy.MaxConcurrent)
			if cfg.App.DNSProxy.Timeout != nil {
				// TODO: Remove this align to milliseconds before release 1.0.0
				t := *cfg.App.DNSProxy.Timeout
				if t < time.Millisecond {
					t *= time.Millisecond
				}
				a.config.DNSProxy.Timeout = t
			}
		}

		if cfg.App.Netfilter != nil {
			if cfg.App.Netfilter.IPTables != nil {
				applyIfSet(&a.config.Netfilter.IPTables.ChainPrefix, cfg.App.Netfilter.IPTables.ChainPrefix)
			}
			if cfg.App.Netfilter.IPSet != nil {
				applyIfSet(&a.config.Netfilter.IPSet.TablePrefix, cfg.App.Netfilter.IPSet.TablePrefix)
				if cfg.App.Netfilter.IPSet.AdditionalTTL != nil {
					// TODO: Remove this align to seconds before release 1.0.0
					t := *cfg.App.Netfilter.IPSet.AdditionalTTL
					if t < time.Second {
						t *= time.Second
					}
					a.config.Netfilter.IPSet.AdditionalTTL = t
				}
			}
			applyIfSet(&a.config.Netfilter.DisableIPv4, cfg.App.Netfilter.DisableIPv4)
			applyIfSet(&a.config.Netfilter.DisableIPv6, cfg.App.Netfilter.DisableIPv6)
			applyIfSet(&a.config.Netfilter.StartMarkTableIndex, cfg.App.Netfilter.StartMarkTableIndex)
		}

		applyIfSet(&a.config.Link, cfg.App.Link)
		applyIfSet(&a.config.ShowAllInterfaces, cfg.App.ShowAllInterfaces)
		applyIfSet(&a.config.LogLevel, cfg.App.LogLevel)
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

		// импортируем новые группы
		for _, group := range *cfg.Groups {
			rules := make([]*models.Rule, len(group.Rules))
			for idx, rule := range group.Rules {
				rules[idx] = &models.Rule{
					ID:     rule.ID,
					Name:   rule.Name,
					Type:   rule.Type,
					Rule:   rule.Rule,
					Enable: rule.Enable,
				}
			}
			if match, _ := colorRegExp.MatchString(group.Color); !match {
				group.Color = "#ffffff"
			} else {
				group.Color = strings.ToLower(group.Color)
			}
			enable := true
			if group.Enable != nil {
				enable = *group.Enable
			}
			err := a.addGroupLocked(&models.Group{
				ID:        group.ID,
				Name:      group.Name,
				Color:     group.Color,
				Interface: group.Interface,
				Enable:    enable,
				Rules:     rules,
			})
			if err != nil {
				return err
			}
		}
	}

	if cfg.Subscriptions != nil {
		a.subscriptions = a.subscriptions[:0]
		for _, sub := range *cfg.Subscriptions {
			enable := true
			if sub.Enable != nil {
				enable = *sub.Enable
			}
			rules := make([]*models.SubscriptionRule, len(sub.Rules))
			for idx, rule := range sub.Rules {
				rules[idx] = &models.SubscriptionRule{
					ID:     rule.ID,
					Rule:   rule.Rule,
					Type:   rule.Type,
					Enable: rule.Enable,
				}
			}
			a.subscriptions = append(a.subscriptions, &models.Subscription{
				ID:         sub.ID,
				Name:       sub.Name,
				Interface:  sub.Interface,
				Enable:     enable,
				URL:        sub.URL,
				Interval:   sub.Interval,
				LastUpdate: sub.LastUpdate,
				Rules:      rules,
			})
		}
	} else {
		a.subscriptions = a.subscriptions[:0]
	}

	return a.syncSubscriptionRuleSetsLocked()
}

func (a *App) ExportConfig() config.Config {
	groupRefs := a.userRuleSetSnapshot()
	groups := make([]config.Group, 0, len(groupRefs))
	for _, group := range groupRefs {
		groupModel := group.Model()
		if groupModel == nil {
			continue
		}
		groupCfg := config.Group{
			ID:        groupModel.ID,
			Name:      groupModel.Name,
			Color:     groupModel.Color,
			Interface: groupModel.Interface,
			Enable:    &groupModel.Enable,
			Rules:     make([]config.Rule, len(groupModel.Rules)),
		}
		for idx, rule := range groupModel.Rules {
			groupCfg.Rules[idx] = config.Rule{
				ID:     rule.ID,
				Name:   rule.Name,
				Type:   rule.Type,
				Rule:   rule.Rule,
				Enable: rule.Enable,
			}
		}
		groups = append(groups, groupCfg)
	}
	subscriptions := a.Subscriptions()

	return config.Config{
		ConfigVersion: constant.Version,
		App: &config.App{
			HTTPWeb: &config.HTTPWeb{
				Enabled: &a.config.HTTPWeb.Enabled,
				Auth: &config.Auth{
					Enabled: &a.config.HTTPWeb.Auth.Enabled,
				},
				Host: &config.HTTPWebServer{
					Address: &a.config.HTTPWeb.Host.Address,
					Port:    &a.config.HTTPWeb.Host.Port,
				},
				Skin: &a.config.HTTPWeb.Skin,
			},
			DNSProxy: &config.DNSProxy{
				Host: &config.DNSProxyServer{
					Address: &a.config.DNSProxy.Host.Address,
					Port:    &a.config.DNSProxy.Host.Port,
				},
				Upstream: &config.DNSProxyServer{
					Address: &a.config.DNSProxy.Upstream.Address,
					Port:    &a.config.DNSProxy.Upstream.Port,
				},
				DisableRemap53:  &a.config.DNSProxy.DisableRemap53,
				DisableFakePTR:  &a.config.DNSProxy.DisableFakePTR,
				DisableDropAAAA: &a.config.DNSProxy.DisableDropAAAA,
				MaxIdleConns:    &a.config.DNSProxy.MaxIdleConns,
				MaxConcurrent:   &a.config.DNSProxy.MaxConcurrent,
				Timeout:         &a.config.DNSProxy.Timeout,
			},
			Netfilter: &config.Netfilter{
				IPTables: &config.IPTables{
					ChainPrefix: &a.config.Netfilter.IPTables.ChainPrefix,
				},
				IPSet: &config.IPSet{
					TablePrefix:   &a.config.Netfilter.IPSet.TablePrefix,
					AdditionalTTL: &a.config.Netfilter.IPSet.AdditionalTTL,
				},
				DisableIPv4:         &a.config.Netfilter.DisableIPv4,
				DisableIPv6:         &a.config.Netfilter.DisableIPv6,
				StartMarkTableIndex: &a.config.Netfilter.StartMarkTableIndex,
			},
			Link:              &a.config.Link,
			ShowAllInterfaces: &a.config.ShowAllInterfaces,
			LogLevel:          &a.config.LogLevel,
		},
		Groups:        &groups,
		Subscriptions: exportSubscriptions(subscriptions),
	}
}

func exportSubscriptions(subs []*models.Subscription) *[]config.Subscription {
	list := make([]config.Subscription, len(subs))
	for idx, sub := range subs {
		rules := make([]config.SubscriptionRule, len(sub.Rules))
		for rIdx, rule := range sub.Rules {
			rules[rIdx] = config.SubscriptionRule{
				ID:     rule.ID,
				Rule:   rule.Rule,
				Type:   rule.Type,
				Enable: rule.Enable,
			}
		}
		list[idx] = config.Subscription{
			ID:         sub.ID,
			Name:       sub.Name,
			Interface:  sub.Interface,
			Enable:     &sub.Enable,
			URL:        sub.URL,
			Interval:   sub.Interval,
			LastUpdate: sub.LastUpdate,
			Rules:      rules,
		}
	}
	return &list
}
