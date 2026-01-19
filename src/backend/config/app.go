package config

type App struct {
	HTTPWeb           *HTTPWeb   `yaml:"httpWeb"`
	DNSProxy          *DNSProxy  `yaml:"dnsProxy"`
	Netfilter         *Netfilter `yaml:"netfilter"`
	Link              *[]string  `yaml:"link"`
	ShowAllInterfaces *bool      `yaml:"showAllInterfaces"`
	LogLevel          *string    `yaml:"logLevel"`
}

type HTTPWeb struct {
	Enabled *bool          `yaml:"enabled"`
	Auth    *Auth          `yaml:"auth"`
	Host    *HTTPWebServer `yaml:"host"`
	Skin    *string        `yaml:"skin"`
}

type Auth struct {
	Enabled *bool `yaml:"enabled"`
}

type HTTPWebServer struct {
	Address *string `yaml:"address"`
	Port    *uint16 `yaml:"port"`
}

type DNSProxy struct {
	Host            *DNSProxyServer `yaml:"host"`
	Upstream        *DNSProxyServer `yaml:"upstream"`
	DisableRemap53  *bool           `yaml:"disableRemap53"`
	DisableFakePTR  *bool           `yaml:"disableFakePTR"`
	DisableDropAAAA *bool           `yaml:"disableDropAAAA"`
}

type DNSProxyServer struct {
	Address *string `yaml:"address"`
	Port    *uint16 `yaml:"port"`
}

type Netfilter struct {
	IPTables            *IPTables `yaml:"iptables"`
	IPSet               *IPSet    `yaml:"ipset"`
	DisableIPv4         *bool     `yaml:"disableIPv4"`
	DisableIPv6         *bool     `yaml:"disableIPv6"`
	StartMarkTableIndex *uint32   `yaml:"startMarkTableIndex"`
}

type IPTables struct {
	ChainPrefix *string `yaml:"chainPrefix"`
}

type IPSet struct {
	TablePrefix   *string `yaml:"tablePrefix"`
	AdditionalTTL *uint32 `yaml:"additionalTTL"`
}
