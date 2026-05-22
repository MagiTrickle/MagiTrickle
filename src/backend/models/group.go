package models

import (
	"magitrickle/utils/intID"
)

type Group struct {
	ID        intID.ID `yaml:"id"`
	Name      string   `yaml:"name"`
	Color     string   `yaml:"color"`
	Interface string   `yaml:"interface"`
	Enable    bool     `yaml:"enable"`
	Rules     []*Rule  `yaml:"rules"`
}
