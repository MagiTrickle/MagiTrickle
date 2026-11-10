package models

import (
	"slices"

	"magitrickle/utils/intID"
)

type SubscriptionRule struct {
	ID     intID.ID `yaml:"id"`
	Rule   string   `yaml:"rule"`
	Type   string   `yaml:"type"`
	Enable bool     `yaml:"enable"`
}

type Subscription struct {
	ID         intID.ID            `yaml:"id"`
	Name       string              `yaml:"name"`
	Interface  string              `yaml:"interface"`
	Enable     bool                `yaml:"enable"`
	URL        string              `yaml:"url"`
	Interval   uint32              `yaml:"interval"`
	LastUpdate uint32              `yaml:"last_update"`
	LastCheck  uint32              `yaml:"-"`
	Rules      []*SubscriptionRule `yaml:"rules"`
}

// Clone возвращает копию подписки с собственным срезом правил. Сами правила
// общие, поэтому менять их на месте нельзя — только заменять в срезе.
func (s *Subscription) Clone() *Subscription {
	clone := *s
	clone.Rules = slices.Clone(s.Rules)
	return &clone
}

// WithID возвращает копию правила с другим ID.
func (r *SubscriptionRule) WithID(id intID.ID) *SubscriptionRule {
	copied := *r
	copied.ID = id
	return &copied
}
