//go:build entware_kn

package interfaces

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const (
	keeneticRCIBaseURL = "http://127.0.0.1:79"
	keeneticRCITimeout = 2 * time.Second
)

type keeneticInterfaceMeta struct {
	Description   string `json:"description"`
	InterfaceName string `json:"interface-name"`
}

func descriptions() (map[string]string, error) {
	client := &http.Client{Timeout: keeneticRCITimeout}

	interfaces, err := keeneticInterfaceList(client)
	if err != nil {
		return nil, err
	}

	friendlyNames := make(map[string]string, len(interfaces))
	for interfaceID, meta := range interfaces {
		systemName, err := keeneticSystemName(client, interfaceID)
		if err != nil || systemName == "" {
			continue
		}

		friendlyName := strings.TrimSpace(meta.Description)
		if friendlyName == "" {
			friendlyName = strings.TrimSpace(meta.InterfaceName)
		}
		if friendlyName == "" || friendlyName == systemName {
			continue
		}

		friendlyNames[systemName] = friendlyName
	}

	return friendlyNames, nil
}

func keeneticInterfaceList(client *http.Client) (map[string]keeneticInterfaceMeta, error) {
	resp, err := client.Get(keeneticRCIBaseURL + "/rci/show/interface")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("unexpected interface list status: %s", resp.Status)
	}

	var payload map[string]json.RawMessage
	if err := json.NewDecoder(resp.Body).Decode(&payload); err != nil {
		return nil, fmt.Errorf("decode interface list: %w", err)
	}

	interfaces := make(map[string]keeneticInterfaceMeta, len(payload))
	for interfaceID, raw := range payload {
		var meta keeneticInterfaceMeta
		if err := json.Unmarshal(raw, &meta); err != nil {
			continue
		}

		interfaces[interfaceID] = meta
	}

	return interfaces, nil
}

func keeneticSystemName(client *http.Client, interfaceID string) (string, error) {
	requestURL := fmt.Sprintf(
		"%s/rci/show/interface/system-name?name=%s",
		keeneticRCIBaseURL,
		url.QueryEscape(interfaceID),
	)

	resp, err := client.Get(requestURL)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("unexpected system-name status: %s", resp.Status)
	}

	var systemName string
	if err := json.NewDecoder(resp.Body).Decode(&systemName); err != nil {
		return "", fmt.Errorf("decode system-name: %w", err)
	}

	return strings.TrimSpace(systemName), nil
}
