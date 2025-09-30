package types

type NetfilterDHookReq struct {
	Type  string `json:"type" example:"iptables"`
	Table string `json:"table" example:"nat"`
}
