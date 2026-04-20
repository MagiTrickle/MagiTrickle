package models

import "magitrickle/utils/intID"

type SubscriptionRule struct {
	ID     intID.ID
	Rule   string
	Type   string
	Enable bool
}

type Subscription struct {
	ID         intID.ID
	Name       string
	Interface  string
	Enable     bool
	URL        string
	Interval   uint32
	LastUpdate uint32
	Rules      []*SubscriptionRule
}
