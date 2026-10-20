package models

import "time"

type Config struct {
	ConfigVersion string           `yaml:"configVersion"`
	App           *AppConfig       `yaml:"app"`
	Groups        *[]*Group        `yaml:"groups"`
	Subscriptions *[]*Subscription `yaml:"subscriptions"`
}

type AppConfig struct {
	HTTPWeb           AppConfigHTTPWeb   `yaml:"httpWeb"`
	DNSProxy          AppConfigDNSProxy  `yaml:"dnsProxy"`
	Netfilter         AppConfigNetfilter `yaml:"netfilter"`
	Link              []string           `yaml:"link"`
	ShowAllInterfaces bool               `yaml:"showAllInterfaces"`
	LogLevel          string             `yaml:"logLevel"`
}

type AppConfigHTTPWeb struct {
	Enabled bool                   `yaml:"enabled"`
	Auth    AppConfigAuth          `yaml:"auth"`
	Host    AppConfigHTTPWebServer `yaml:"host"`
	Skin    string                 `yaml:"skin"`
}

type AppConfigAuth struct {
	Enabled bool `yaml:"enabled"`
}

type AppConfigHTTPWebServer struct {
	Address string `yaml:"address"`
	Port    uint16 `yaml:"port"`
}

type AppConfigDNSProxy struct {
	Host            AppConfigDNSProxyServer `yaml:"host"`
	Upstream        AppConfigDNSProxyServer `yaml:"upstream"`
	DisableRemap53  bool                    `yaml:"disableRemap53"`
	DisableFakePTR  bool                    `yaml:"disableFakePTR"`
	DisableDropAAAA bool                    `yaml:"disableDropAAAA"`
	MaxIdleConns    uint                    `yaml:"maxIdleConns"`
	MaxConcurrent   uint                    `yaml:"maxConcurrent"`
	Timeout         time.Duration           `yaml:"timeout"`
}

type AppConfigDNSProxyServer struct {
	Address string `yaml:"address"`
	Port    uint16 `yaml:"port"`
}

type AppConfigNetfilter struct {
	IPTables            AppConfigIPTables `yaml:"iptables"`
	IPSet               AppConfigIPSet    `yaml:"ipset"`
	DisableIPv4         bool              `yaml:"disableIPv4"`
	DisableIPv6         bool              `yaml:"disableIPv6"`
	StartMarkTableIndex uint32            `yaml:"startMarkTableIndex"`
}

type AppConfigIPTables struct {
	ChainPrefix string `yaml:"chainPrefix"`
}

type AppConfigIPSet struct {
	TablePrefix   string        `yaml:"tablePrefix"`
	AdditionalTTL time.Duration `yaml:"additionalTTL"`
}
