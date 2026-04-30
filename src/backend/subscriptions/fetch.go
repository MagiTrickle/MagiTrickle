package subscriptions

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

const fetchTimeout = 15 * time.Second
const maxFetchRedirects = 5

func FetchList(rawURL string) (string, error) {
	parsed, err := url.Parse(rawURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return "", fmt.Errorf("invalid url")
	}
	if !isSupportedFetchURL(parsed) {
		return "", fmt.Errorf("unsupported url scheme")
	}

	client := &http.Client{
		Timeout: fetchTimeout,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	visited := map[string]struct{}{parsed.String(): {}}

	for redirects := 0; ; redirects++ {
		resp, err := client.Get(parsed.String())
		if err != nil {
			return "", err
		}

		if resp.StatusCode == http.StatusMovedPermanently || resp.StatusCode == http.StatusFound {
			location, err := resp.Location()
			resp.Body.Close()
			if err != nil {
				return "", fmt.Errorf("bad redirect location: %w", err)
			}
			if !location.IsAbs() {
				location = parsed.ResolveReference(location)
			}
			if !isSupportedFetchURL(location) {
				return "", fmt.Errorf("unsupported redirect url")
			}
			if _, exists := visited[location.String()]; exists {
				return "", fmt.Errorf("redirect loop detected")
			}
			if redirects >= maxFetchRedirects {
				return "", fmt.Errorf("too many redirects")
			}
			visited[location.String()] = struct{}{}
			parsed = location
			continue
		}

		defer resp.Body.Close()
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return "", fmt.Errorf("bad response status: %d", resp.StatusCode)
		}

		data, err := io.ReadAll(resp.Body)
		if err != nil {
			return "", err
		}
		return string(data), nil
	}
}

func isSupportedFetchURL(parsed *url.URL) bool {
	return parsed != nil && parsed.Host != "" && (parsed.Scheme == "http" || parsed.Scheme == "https")
}
