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
	ID         *intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name       string    `json:"name" example:"Subscription"`
	Interface  string    `json:"interface" example:"nwg0"`
	Enable     *bool     `json:"enable" example:"true" TODO:"Make required after 1.0.0"`
	URL        string    `json:"url" example:"https://example.com/list.txt"`
	LastUpdate *int64    `json:"last_update" example:"1700000000000"`
	SubscriptionRulesReq
}

type SubscriptionRes struct {
	ID         intID.ID `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name       string   `json:"name" example:"Subscription"`
	Interface  string   `json:"interface" example:"nwg0"`
	Enable     bool     `json:"enable" example:"true"`
	URL        string   `json:"url" example:"https://example.com/list.txt"`
	LastUpdate int64    `json:"last_update" example:"1700000000000"`
	SubscriptionRulesRes
}
