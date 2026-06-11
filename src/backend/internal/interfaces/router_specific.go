package interfaces

type RouterSpecificAPI interface {
	GetIfaceAliases() (map[string]string, error)
}

type DummyRouterSpecificAPI struct{}

func (DummyRouterSpecificAPI) GetIfaceAliases() (map[string]string, error) {
	return map[string]string{}, nil
}
