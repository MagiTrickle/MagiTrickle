package types

import "magitrickle/utils/intID"

type SubscriptionsReq struct {
	Subscriptions *[]SubscriptionReq `json:"subscriptions"`
}

type SubscriptionsRes struct {
	Subscriptions *[]SubscriptionRes `json:"subscriptions,omitempty"`
}

type SubscriptionRulesReq struct {
	Rules *[]SubscriptionRuleReq `json:"rules"`
}

type SubscriptionRulesRes struct {
	Rules *[]SubscriptionRuleRes `json:"rules,omitempty"`
}

type SubscriptionSyncReq struct {
	URL string `json:"url,omitempty" example:"https://example.com/list.txt"`
}

type SubscriptionRuleReq struct {
	ID     *intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Rule   string    `json:"rule" example:"example.com"`
	Type   string    `json:"type" example:"domain"`
	Enable bool      `json:"enable" example:"true"`
}

type SubscriptionRuleRes struct {
	ID     intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Rule   string   `json:"rule" example:"example.com"`
	Type   string   `json:"type" example:"domain"`
	Enable bool     `json:"enable" example:"true"`
}

type SubscriptionReq struct {
	ID        *intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name      string    `json:"name" example:"Subscription"`
	Interface string    `json:"interface" example:"nwg0"`
	// TODO: Make required after 1.0.0.
	Enable     *bool   `json:"enable" example:"true"`
	URL        string  `json:"url" example:"https://example.com/list.txt"`
	Interval   *uint32 `json:"interval" example:"86400"`
	LastUpdate *uint32 `json:"lastUpdate" example:"1700000000"`
	SubscriptionRulesReq
}

type SubscriptionRes struct {
	ID         intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name       string   `json:"name" example:"Subscription"`
	Interface  string   `json:"interface" example:"nwg0"`
	Enable     bool     `json:"enable" example:"true"`
	URL        string   `json:"url" example:"https://example.com/list.txt"`
	Interval   uint32   `json:"interval" example:"86400"`
	LastUpdate uint32   `json:"lastUpdate" example:"1700000000"`
	SubscriptionRulesRes
}
