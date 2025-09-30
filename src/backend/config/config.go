package config

type Config struct {
	ConfigVersion string   `yaml:"configVersion"`
	App           *App     `yaml:"app"`
	Groups        *[]Group `yaml:"groups"`
}
