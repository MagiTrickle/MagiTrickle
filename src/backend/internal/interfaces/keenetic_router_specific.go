//go:build entware_kn

package interfaces

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"strings"
	"time"
)

const (
	keeneticRCIBaseURL = "http://127.0.0.1:79"
	keeneticRCITimeout = 2 * time.Second
)

type KeeneticRouterSpecificAPI struct {
	BaseURL string
	Client  *http.Client
}

type keeneticInterfaceMeta struct {
	Description   string `json:"description"`
	InterfaceName string `json:"interface-name"`
}

type keeneticInterfaceSystemNameRequest struct {
	Show struct {
		Interface struct {
			Name       string `json:"name"`
			Details    string `json:"details"`
			SystemName string `json:"system-name"`
		} `json:"interface"`
	} `json:"show"`
}

type keeneticInterfaceSystemNameResponse struct {
	Show struct {
		Interface struct {
			SystemName string `json:"system-name"`
		} `json:"interface"`
	} `json:"show"`
}

func NewKeeneticRouterSpecificAPI() *KeeneticRouterSpecificAPI {
	return &KeeneticRouterSpecificAPI{
		BaseURL: keeneticRCIBaseURL,
		Client:  &http.Client{Timeout: keeneticRCITimeout},
	}
}

func (a *KeeneticRouterSpecificAPI) GetIfaceAliases() (map[string]string, error) {
	client := a.httpClient()
	baseURL := a.baseURL()

	interfaces, err := a.interfaceList(client, baseURL)
	if err != nil {
		return nil, err
	}

	interfaceIDs := make([]string, 0, len(interfaces))
	for interfaceID := range interfaces {
		interfaceIDs = append(interfaceIDs, interfaceID)
	}
	sort.Strings(interfaceIDs) // map order would make the choice between duplicate system names random

	systemNames, err := a.interfaceSystemNames(client, baseURL, interfaceIDs)
	if err != nil {
		return nil, err
	}

	aliases := make(map[string]string, len(systemNames))
	fromDescription := make(map[string]bool, len(systemNames))
	for _, interfaceID := range interfaceIDs {
		systemName := systemNames[interfaceID]
		if systemName == "" {
			continue
		}

		meta := interfaces[interfaceID]
		alias := strings.TrimSpace(meta.Description)
		described := alias != ""
		if !described {
			alias = strings.TrimSpace(meta.InterfaceName)
		}
		if alias == "" || alias == systemName {
			continue
		}

		// Several Keenetic interfaces can share one system name (a Port "0" is the same eth3 as
		// GigabitEthernet1); a user-visible description must beat a bare port label.
		if _, taken := aliases[systemName]; taken && (fromDescription[systemName] || !described) {
			continue
		}
		aliases[systemName] = alias
		fromDescription[systemName] = described
	}

	return aliases, nil
}

func (a *KeeneticRouterSpecificAPI) httpClient() *http.Client {
	if a != nil && a.Client != nil {
		return a.Client
	}
	return &http.Client{Timeout: keeneticRCITimeout}
}

func (a *KeeneticRouterSpecificAPI) baseURL() string {
	if a != nil && a.BaseURL != "" {
		return strings.TrimRight(a.BaseURL, "/")
	}
	return keeneticRCIBaseURL
}

func (a *KeeneticRouterSpecificAPI) interfaceList(client *http.Client, baseURL string) (map[string]keeneticInterfaceMeta, error) {
	resp, err := client.Get(baseURL + "/rci/show/interface")
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

func (a *KeeneticRouterSpecificAPI) interfaceSystemNames(client *http.Client, baseURL string, interfaceIDs []string) (map[string]string, error) {
	if len(interfaceIDs) == 0 {
		return map[string]string{}, nil
	}

	requests := make([]keeneticInterfaceSystemNameRequest, len(interfaceIDs))
	for i, interfaceID := range interfaceIDs {
		requests[i].Show.Interface.Name = interfaceID
		requests[i].Show.Interface.Details = "yes"
		requests[i].Show.Interface.SystemName = "yes"
	}

	body, err := json.Marshal(requests)
	if err != nil {
		return nil, fmt.Errorf("encode interface system-name request: %w", err)
	}

	resp, err := client.Post(baseURL+"/rci/", "application/json", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("unexpected system-name status: %s", resp.Status)
	}

	var payload []keeneticInterfaceSystemNameResponse
	if err := json.NewDecoder(resp.Body).Decode(&payload); err != nil {
		return nil, fmt.Errorf("decode system-name: %w", err)
	}

	systemNames := make(map[string]string, len(interfaceIDs))
	for i, item := range payload {
		if i >= len(interfaceIDs) {
			break
		}

		systemName := strings.TrimSpace(item.Show.Interface.SystemName)
		if systemName == "" {
			continue
		}

		systemNames[interfaceIDs[i]] = systemName
	}

	return systemNames, nil
}
