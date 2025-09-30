package types

type InterfacesRes struct {
	Interfaces []InterfaceRes `json:"interfaces,omitempty"`
}

type InterfaceRes struct {
	ID string `json:"id" example:"nwg0" swaggertype:"string"`
}
