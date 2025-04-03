package types

type RulesReq struct {
	Rules *[]RuleReq `json:"rules"`
}

type RulesRes struct {
	Rules *[]RuleRes `json:"rules,omitempty"`
}

type RuleReq struct {
	ID     *ID    `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name   string `json:"name" example:"Example Domain"`
	Type   string `json:"type" example:"domain"`
	Rule   string `json:"rule" example:"example.com"`
	Enable bool   `json:"enable" example:"true"`
}

type RuleRes struct {
	ID     ID     `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name   string `json:"name" example:"Example Domain"`
	Type   string `json:"type" example:"domain"`
	Rule   string `json:"rule" example:"example.com"`
	Enable bool   `json:"enable" example:"true"`
}
