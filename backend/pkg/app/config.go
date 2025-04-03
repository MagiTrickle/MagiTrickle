package app

import (
	"errors"
	"fmt"
	"os"
	"regexp"
	"strings"

	"github.com/Ponywka/MagiTrickle/backend/constant"
	"github.com/Ponywka/MagiTrickle/backend/models"
	"github.com/Ponywka/MagiTrickle/backend/models/config"

	"gopkg.in/yaml.v3"
)

var colorRegExp = regexp.MustCompile(`^#[0-9A-Fa-f]{6}$`)

const cfgFolderLocation = constant.AppDataDir
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

func (a *App) ImportConfig(cfg config.Config) error {
	if !strings.HasPrefix(cfg.ConfigVersion, "0.1.") {
		return ErrConfigUnsupportedVersion
	}

	if cfg.App != nil {
		if cfg.App.HTTPWeb != nil {
			if cfg.App.HTTPWeb.Enabled != nil {
				a.config.HTTPWeb.Enabled = *cfg.App.HTTPWeb.Enabled
			}
			if cfg.App.HTTPWeb.Host != nil {
				if cfg.App.HTTPWeb.Host.Address != nil {
					a.config.HTTPWeb.Host.Address = *cfg.App.HTTPWeb.Host.Address
				}
				if cfg.App.HTTPWeb.Host.Port != nil {
					a.config.HTTPWeb.Host.Port = *cfg.App.HTTPWeb.Host.Port
				}
			}
			if cfg.App.HTTPWeb.Skin != nil {
				a.config.HTTPWeb.Skin = *cfg.App.HTTPWeb.Skin
			}
		}

		if cfg.App.DNSProxy != nil {
			if cfg.App.DNSProxy.Upstream != nil {
				if cfg.App.DNSProxy.Upstream.Address != nil {
					a.config.DNSProxy.Upstream.Address = *cfg.App.DNSProxy.Upstream.Address
				}
				if cfg.App.DNSProxy.Upstream.Port != nil {
					a.config.DNSProxy.Upstream.Port = *cfg.App.DNSProxy.Upstream.Port
				}
			}
			if cfg.App.DNSProxy.Host != nil {
				if cfg.App.DNSProxy.Host.Address != nil {
					a.config.DNSProxy.Host.Address = *cfg.App.DNSProxy.Host.Address
				}
				if cfg.App.DNSProxy.Host.Port != nil {
					a.config.DNSProxy.Host.Port = *cfg.App.DNSProxy.Host.Port
				}
			}
			if cfg.App.DNSProxy.DisableRemap53 != nil {
				a.config.DNSProxy.DisableRemap53 = *cfg.App.DNSProxy.DisableRemap53
			}
			if cfg.App.DNSProxy.DisableFakePTR != nil {
				a.config.DNSProxy.DisableFakePTR = *cfg.App.DNSProxy.DisableFakePTR
			}
			if cfg.App.DNSProxy.DisableDropAAAA != nil {
				a.config.DNSProxy.DisableDropAAAA = *cfg.App.DNSProxy.DisableDropAAAA
			}
		}

		if cfg.App.Netfilter != nil {
			if cfg.App.Netfilter.IPTables != nil {
				if cfg.App.Netfilter.IPTables.ChainPrefix != nil {
					a.config.Netfilter.IPTables.ChainPrefix = *cfg.App.Netfilter.IPTables.ChainPrefix
				}
			}
			if cfg.App.Netfilter.IPSet != nil {
				if cfg.App.Netfilter.IPSet.TablePrefix != nil {
					a.config.Netfilter.IPSet.TablePrefix = *cfg.App.Netfilter.IPSet.TablePrefix
				}
				if cfg.App.Netfilter.IPSet.AdditionalTTL != nil {
					a.config.Netfilter.IPSet.AdditionalTTL = *cfg.App.Netfilter.IPSet.AdditionalTTL
				}
			}
			if cfg.App.Netfilter.DisableIPv4 != nil {
				a.config.Netfilter.DisableIPv4 = *cfg.App.Netfilter.DisableIPv4
			}
			if cfg.App.Netfilter.DisableIPv6 != nil {
				a.config.Netfilter.DisableIPv6 = *cfg.App.Netfilter.DisableIPv6
			}
		}

		if cfg.App.Link != nil {
			a.config.Link = *cfg.App.Link
		}

		if cfg.App.LogLevel != nil {
			a.config.LogLevel = *cfg.App.LogLevel
		}
	}

	if cfg.Groups != nil {
		// отключаем старые группы и очищаем срез
		for _, group := range a.groups {
			_ = group.Disable()
		}
		a.groups = a.groups[:0]

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
			if !colorRegExp.MatchString(group.Color) {
				group.Color = "#ffffff"
			}
			// TODO: Make required after 1.0.0
			enable := true
			if group.Enable != nil {
				enable = *group.Enable
			}
			err := a.AddGroup(&models.Group{
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

	return nil
}

func (a *App) ExportConfig() config.Config {
	groups := make([]config.Group, len(a.groups))
	for idx, group := range a.groups {
		groupCfg := config.Group{
			ID:        group.ID,
			Name:      group.Name,
			Color:     group.Color,
			Interface: group.Interface,
			Enable:    &group.Group.Enable,
			Rules:     make([]config.Rule, len(group.Rules)),
		}
		for idx, rule := range group.Rules {
			groupCfg.Rules[idx] = config.Rule{
				ID:     rule.ID,
				Name:   rule.Name,
				Type:   rule.Type,
				Rule:   rule.Rule,
				Enable: rule.Enable,
			}
		}
		groups[idx] = groupCfg
	}

	return config.Config{
		ConfigVersion: "0.1.2",
		App: &config.App{
			HTTPWeb: &config.HTTPWeb{
				Enabled: &a.config.HTTPWeb.Enabled,
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
			},
			Netfilter: &config.Netfilter{
				IPTables: &config.IPTables{
					ChainPrefix: &a.config.Netfilter.IPTables.ChainPrefix,
				},
				IPSet: &config.IPSet{
					TablePrefix:   &a.config.Netfilter.IPSet.TablePrefix,
					AdditionalTTL: &a.config.Netfilter.IPSet.AdditionalTTL,
				},
				DisableIPv4: &a.config.Netfilter.DisableIPv4,
				DisableIPv6: &a.config.Netfilter.DisableIPv6,
			},
			Link:     &a.config.Link,
			LogLevel: &a.config.LogLevel,
		},
		Groups: &groups,
	}
}
