package auth

import (
	"bufio"
	"errors"
	"fmt"
	"magitrickle/constant"
	"os"
	"strings"
)

func loadPasswordHash(login string) (string, error) {
	filePath := constant.ShadowFile
	if _, err := os.Stat(constant.ShadowFile); err != nil {
		if os.IsNotExist(err) {
			filePath = constant.PasswdFile
		}
	}

	file, err := os.Open(filePath)
	if err != nil {
		return "", fmt.Errorf("failed to open %s: %w", filePath, err)
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
		return "", fmt.Errorf("failed to read %s: %w", filePath, err)
	}
	return "", errors.New("user not found")
}
