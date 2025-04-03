package api

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"

	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
)

type Client struct {
	client *http.Client
}

func (c Client) NetfilterDHook(ipttype, table string) error {
	body := types.NetfilterDHookReq{
		Type:  ipttype,
		Table: table,
	}
	bodyJSON, err := json.Marshal(body)
	if err != nil {
		return fmt.Errorf("marshalling failed: %w", err)
	}

	req, err := http.NewRequest("POST", "http://unix/api/v1/system/hooks/netfilterd", bytes.NewReader(bodyJSON))
	if err != nil {
		return fmt.Errorf("creating request failed: %w", err)
	}

	resp, err := c.client.Do(req)
	if err != nil {
		return fmt.Errorf("executing request failed: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()

	return nil
}

func NewClient() Client {
	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(_ context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", SocketPath)
			},
		},
	}
	return Client{client: client}
}
