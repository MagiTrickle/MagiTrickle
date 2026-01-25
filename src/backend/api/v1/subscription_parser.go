package v1

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"

	"magitrickle/models"
	"magitrickle/utils/intID"

	"github.com/dlclark/regexp2"
)

const subscriptionListMaxBytes = 5 * 1024 * 1024

var (
	domainCharRe   = regexp.MustCompile(`^[a-zA-Z0-9-.]+$`)
	wildcardCharRe = regexp.MustCompile(`^[a-zA-Z0-9\-.*?]+$`)
	subnetRe       = regexp.MustCompile(`^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$`)
)

func fetchSubscriptionList(rawURL string) (string, error) {
	parsed, err := url.Parse(rawURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return "", fmt.Errorf("invalid url")
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return "", fmt.Errorf("unsupported url scheme")
	}

	client := &http.Client{Timeout: 10 * time.Second}
	resp, err := client.Get(rawURL)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("bad response status: %d", resp.StatusCode)
	}

	limited := io.LimitReader(resp.Body, subscriptionListMaxBytes+1)
	data, err := io.ReadAll(limited)
	if err != nil {
		return "", err
	}
	if int64(len(data)) > subscriptionListMaxBytes {
		return "", errors.New("subscription list too large")
	}
	return string(data), nil
}

func parseSubscriptionRulesFromList(list string) []*models.SubscriptionRule {
	rules := make([]*models.SubscriptionRule, 0)
	seen := make(map[string]struct{})
	parts := strings.FieldsFunc(list, func(r rune) bool {
		return r == '\n' || r == ',' || r == '\r'
	})
	for _, part := range parts {
		line := strings.TrimSpace(part)
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "#") {
			continue
		}

		ruleType := detectSubscriptionRuleType(line)
		key := ruleType + "|" + line
		if _, exists := seen[key]; exists {
			continue
		}
		seen[key] = struct{}{}

		rules = append(rules, &models.SubscriptionRule{
			ID:     intID.RandomID(),
			Rule:   line,
			Type:   ruleType,
			Enable: true,
		})
	}
	return rules
}

func detectSubscriptionRuleType(pattern string) string {
	p := strings.TrimSpace(pattern)

	if isValidSubnet6(p) {
		return "subnet6"
	}
	if isValidSubnet(p) {
		return "subnet"
	}
	if strings.Count(p, ".") >= 2 && isValidDomain(p) {
		return "domain"
	}
	if isValidNamespace(p) {
		return "namespace"
	}
	if isValidRegex(p) {
		return "regex"
	}
	if isValidWildcard(p) {
		return "wildcard"
	}

	return "domain"
}

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
	return wildcardCharRe.MatchString(pattern)
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
	return domainCharRe.MatchString(pattern)
}

func isValidNamespace(pattern string) bool {
	return isValidDomain(pattern)
}

func isValidSubnet(pattern string) bool {
	matches := subnetRe.FindStringSubmatch(pattern)
	if len(matches) == 0 {
		return false
	}
	for i := 1; i <= 4; i++ {
		if n := toInt(matches[i]); n < 0 || n > 255 {
			return false
		}
	}
	if matches[5] != "" {
		if n := toInt(matches[5]); n < 0 || n > 32 {
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
