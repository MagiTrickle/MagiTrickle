//go:build !entware_kn

package interfaces

func initRouterSpecificAPI() RouterSpecificAPI {
	return DummyRouterSpecificAPI{}
}
