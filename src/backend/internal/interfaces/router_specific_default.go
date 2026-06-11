//go:build !entware_kn

package interfaces

func routerSpecificAPI() RouterSpecificAPI {
	return DummyRouterSpecificAPI{}
}
