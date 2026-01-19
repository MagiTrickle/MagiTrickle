//go:build entware

package auth

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"
)

const passwdPath = "/opt/etc/passwd"

func loadPasswordHash(login string) (string, error) {
	file, err := os.Open(passwdPath)
	if err != nil {
		return "", fmt.Errorf("failed to open passwd: %w", err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Split(line, ":")
		if len(parts) < 2 {
			continue
		}
		if parts[0] != login {
			continue
		}
		hash := strings.TrimSpace(parts[1])
		if hash == "" || hash == "x" || hash == "*" {
			return "", errors.New("user has no password")
		}
		return hash, nil
	}
	if err := scanner.Err(); err != nil {
		return "", fmt.Errorf("failed to read passwd: %w", err)
	}
	return "", errors.New("user not found")
}
