package config

import "magitrickle/utils/intID"

type SubscriptionRule struct {
	ID     intID.ID `yaml:"id"`
	Rule   string   `yaml:"rule"`
	Type   string   `yaml:"type"`
	Enable bool     `yaml:"enable"`
}

type Subscription struct {
	ID         intID.ID           `yaml:"id"`
	Name       string             `yaml:"name"`
	Interface  string             `yaml:"interface"`
	Enable     *bool              `yaml:"enable"`
	URL        string             `yaml:"url"`
	Interval   uint32             `yaml:"interval"`
	LastUpdate int64              `yaml:"last_update"`
	Rules      []SubscriptionRule `yaml:"rules"`
}
