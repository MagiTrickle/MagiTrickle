package config

import (
	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
)

type Rule struct {
	ID     types.ID `yaml:"id"`
	Name   string   `yaml:"name"`
	Type   string   `yaml:"type"`
	Rule   string   `yaml:"rule"`
	Enable bool     `yaml:"enable"`
}
