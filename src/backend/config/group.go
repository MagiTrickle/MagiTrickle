package config

import (
	"magitrickle/utils/intID"
)

type Group struct {
	ID        intID.ID `yaml:"id"`
	Name      string   `yaml:"name"`
	Color     string   `yaml:"color"`
	Interface string   `yaml:"interface"`
	Enable    *bool    `yaml:"enable"` // TODO: Make required after 1.0.0
	Rules     []Rule   `yaml:"rules"`
}

type Rule struct {
	ID     intID.ID `yaml:"id"`
	Name   string   `yaml:"name"`
	Type   string   `yaml:"type"`
	Rule   string   `yaml:"rule"`
	Enable bool     `yaml:"enable"`
}
