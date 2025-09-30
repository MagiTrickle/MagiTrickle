package constant

import (
	"magitrickle/models"
)

const (
	DefaultSocketPath = "/opt/var/run/magitrickle.sock"
)

var DefaultAppConfig = models.AppConfig{
	DNSProxy: models.AppConfigDNSProxy{
		Host:            models.AppConfigDNSProxyServer{Address: "[::]", Port: 3553},
		Upstream:        models.AppConfigDNSProxyServer{Address: "127.0.0.1", Port: 53},
		DisableRemap53:  false,
		DisableFakePTR:  false,
		DisableDropAAAA: false,
	},
	HTTPWeb: models.AppConfigHTTPWeb{
		Enabled: true,
		Host: models.AppConfigHTTPWebServer{
			Address: "[::]",
			Port:    8080,
		},
		Skin: "default",
	},
	Netfilter: models.AppConfigNetfilter{
		IPTables: models.AppConfigIPTables{
			ChainPrefix: "MT_",
		},
		IPSet: models.AppConfigIPSet{
			TablePrefix:   "mt_",
			AdditionalTTL: 3600,
		},
		DisableIPv4:         false,
		DisableIPv6:         false,
		StartMarkTableIndex: 0x4D616769, // Magi
	},
	Link:              []string{"br0"},
	ShowAllInterfaces: false,
	LogLevel:          "info",
}

var (
	Version = "unattached"
)
