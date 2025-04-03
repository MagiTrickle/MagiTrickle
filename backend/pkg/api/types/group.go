package types

type GroupsReq struct {
	Groups *[]GroupReq `json:"groups"`
}

type GroupsRes struct {
	Groups *[]GroupRes `json:"groups,omitempty"`
}

type GroupReq struct {
	ID        *ID    `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name      string `json:"name" example:"Routing"`
	Color     string `json:"color" example:"#ffffff"`
	Interface string `json:"interface" example:"nwg0"`
	Enable    *bool  `json:"enable" example:"true" TODO:"Make required after 1.0.0"`
	RulesReq
}

type GroupRes struct {
	ID        ID     `json:"id" example:"0a1b2c3d" swaggertype:"string"`
	Name      string `json:"name" example:"Routing"`
	Color     string `json:"color" example:"#ffffff"`
	Interface string `json:"interface" example:"nwg0"`
	Enable    bool   `json:"enable" example:"true"`
	RulesRes
}
