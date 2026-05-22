package config

import "magitrickle/models"

type Config struct {
	ConfigVersion string                  `yaml:"configVersion"`
	App           *App                    `yaml:"app"`
	Groups        *[]*models.Group        `yaml:"groups"`
	Subscriptions *[]*models.Subscription `yaml:"subscriptions"`
}
