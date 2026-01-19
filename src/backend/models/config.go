package models

type AppConfig struct {
	HTTPWeb           AppConfigHTTPWeb
	DNSProxy          AppConfigDNSProxy
	Netfilter         AppConfigNetfilter
	Link              []string
	ShowAllInterfaces bool
	LogLevel          string
}

type AppConfigHTTPWeb struct {
	Enabled bool
	Auth    AppConfigAuth
	Host    AppConfigHTTPWebServer
	Skin    string
}

type AppConfigAuth struct {
	Enabled bool
}

type AppConfigHTTPWebServer struct {
	Address string
	Port    uint16
}

type AppConfigDNSProxy struct {
	Host            AppConfigDNSProxyServer
	Upstream        AppConfigDNSProxyServer
	DisableRemap53  bool
	DisableFakePTR  bool
	DisableDropAAAA bool
}

type AppConfigDNSProxyServer struct {
	Address string
	Port    uint16
}

type AppConfigNetfilter struct {
	IPTables            AppConfigIPTables
	IPSet               AppConfigIPSet
	DisableIPv4         bool
	DisableIPv6         bool
	StartMarkTableIndex uint32
}

type AppConfigIPTables struct {
	ChainPrefix string
}

type AppConfigIPSet struct {
	TablePrefix   string
	AdditionalTTL uint32
}
