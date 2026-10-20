package models

import (
	"slices"

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

// Clone возвращает копию группы с собственным срезом правил. Сами правила
// общие, поэтому менять их на месте нельзя — только заменять в срезе.
func (g *Group) Clone() *Group {
	clone := *g
	clone.Rules = slices.Clone(g.Rules)
	return &clone
}
