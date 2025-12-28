//go:build openwrt

package constant

const (
	AppConfigDir = "/etc/magitrickle"
	AppShareDir  = "/usr/share/magitrickle"
	AppStateDir  = "/etc/magitrickle/state"
	PIDPath      = "/var/run/magitrickle.pid"
	SockPath     = "/var/run/magitrickle.sock"
)
