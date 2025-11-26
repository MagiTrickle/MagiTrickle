//go:build !entware

package constant

const (
	AppConfigDir = "/etc/magitrickle"
	AppShareDir  = "/usr/share/magitrickle"
	AppDataDir   = "/var/lib/magitrickle"
	PIDPath      = "/var/run/magitrickle.pid"
	SockPath     = "/var/run/magitrickle.sock"
)
