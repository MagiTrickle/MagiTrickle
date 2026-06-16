package interfaces

type RouterSpecificAPI interface {
	GetIfaceAliases() (map[string]string, error)
}

var routerAPI RouterSpecificAPI

func init() {
	routerAPI = initRouterSpecificAPI()
}

type DummyRouterSpecificAPI struct{}

func (DummyRouterSpecificAPI) GetIfaceAliases() (map[string]string, error) {
	return map[string]string{}, nil
}
