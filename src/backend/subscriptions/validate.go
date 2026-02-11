package subscriptions

import (
	"strings"

	"github.com/dlclark/regexp2"
)

var (
	domainCharRe   = regexp2.MustCompile(`^[a-zA-Z0-9-.]+$`, 0)
	wildcardCharRe = regexp2.MustCompile(`^[a-zA-Z0-9\-.*?]+$`, 0)
	subnetRe       = regexp2.MustCompile(`^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$`, 0)
)

func isValidWildcard(pattern string) bool {
	if pattern == "" {
		return false
	}
	if strings.HasPrefix(pattern, ".") || strings.HasSuffix(pattern, ".") {
		return false
	}
	if strings.Contains(pattern, "..") || strings.Contains(pattern, "**") {
		return false
	}
	ok, _ := wildcardCharRe.MatchString(pattern)
	return ok
}

func isValidDomain(pattern string) bool {
	if pattern == "" {
		return false
	}
	if strings.HasPrefix(pattern, ".") || strings.HasSuffix(pattern, ".") {
		return false
	}
	if strings.Contains(pattern, "..") {
		return false
	}
	ok, _ := domainCharRe.MatchString(pattern)
	return ok
}

func isValidNamespace(pattern string) bool {
	return isValidDomain(pattern)
}

func isValidSubnet(pattern string) bool {
	match, _ := subnetRe.FindStringMatch(pattern)
	if match == nil {
		return false
	}

	groups := match.Groups()
	if len(groups) < 5 {
		return false
	}

	for i := 1; i <= 4; i++ {
		if n := toInt(groups[i].String()); n < 0 || n > 255 {
			return false
		}
	}
	if len(groups) > 5 && groups[5].String() != "" {
		if n := toInt(groups[5].String()); n < 0 || n > 32 {
			return false
		}
	}
	return true
}

func isValidSubnet6(pattern string) bool {
	parts := strings.Split(pattern, "/")
	if len(parts) == 1 {
		return isValidIPv6(parts[0])
	}
	if len(parts) != 2 {
		return false
	}
	prefix := toInt(parts[1])
	if prefix < 0 || prefix > 128 {
		return false
	}
	return isValidIPv6(parts[0])
}

func isValidIPv6(ip string) bool {
	if !strings.Contains(ip, ":") {
		return false
	}
	for _, r := range ip {
		if !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f' || r >= 'A' && r <= 'F' || r == ':') {
			return false
		}
	}
	return true
}

func isValidRegex(pattern string) bool {
	re, err := regexp2.Compile(pattern, 0)
	return err == nil && re != nil
}

func toInt(s string) int {
	if s == "" {
		return -1
	}
	var n int
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return -1
		}
		n = n*10 + int(c-'0')
	}
	return n
}
