package config

import (
	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
)

type Group struct {
	ID        types.ID `yaml:"id"`
	Name      string   `yaml:"name"`
	Color     string   `yaml:"color"`
	Interface string   `yaml:"interface"`
	Enable    *bool    `yaml:"enable"` // TODO: Make required after 1.0.0
	Rules     []Rule   `yaml:"rules"`
}
