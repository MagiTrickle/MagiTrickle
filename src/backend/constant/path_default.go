//go:build !entware && !openwrt

package constant

const (
	AppConfigDir = "/etc/magitrickle"
	AppShareDir  = "/usr/share/magitrickle"
	AppStateDir  = "/var/lib/magitrickle"
	PIDPath      = "/var/run/magitrickle.pid"
	SockPath     = "/var/run/magitrickle.sock"
)
