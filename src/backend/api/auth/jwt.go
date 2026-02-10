package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

type jwtHeader struct {
	Alg string `json:"alg"`
	Typ string `json:"typ"`
}

type jwtClaims struct {
	Sub string `json:"sub"`
	Iss string `json:"iss"`
	Iat int64  `json:"iat"`
	Exp int64  `json:"exp"`
}

func signJWT(header jwtHeader, claims jwtClaims, secret []byte) (string, error) {
	headerBytes, err := json.Marshal(header)
	if err != nil {
		return "", fmt.Errorf("failed to marshal header: %w", err)
	}
	claimsBytes, err := json.Marshal(claims)
	if err != nil {
		return "", fmt.Errorf("failed to marshal claims: %w", err)
	}
	encodedHeader := base64.RawURLEncoding.EncodeToString(headerBytes)
	encodedClaims := base64.RawURLEncoding.EncodeToString(claimsBytes)
	payload := encodedHeader + "." + encodedClaims

	signature := hmacSHA256(payload, secret)
	return payload + "." + signature, nil
}

func parseAndVerifyJWT(token string, secret []byte) (jwtClaims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return jwtClaims{}, errors.New("invalid token format")
	}
	payload := parts[0] + "." + parts[1]
	expectedSig := hmacSHA256(payload, secret)
	if !hmac.Equal([]byte(expectedSig), []byte(parts[2])) {
		return jwtClaims{}, errors.New("invalid token signature")
	}

	return parseClaims(parts[1])
}

func parseJWTWithoutVerification(token string) (jwtClaims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return jwtClaims{}, errors.New("invalid token format")
	}
	return parseClaims(parts[1])
}

func parseClaims(encoded string) (jwtClaims, error) {
	claimsBytes, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil {
		return jwtClaims{}, fmt.Errorf("failed to decode claims: %w", err)
	}
	var claims jwtClaims
	if err := json.Unmarshal(claimsBytes, &claims); err != nil {
		return jwtClaims{}, fmt.Errorf("failed to parse claims: %w", err)
	}
	return claims, nil
}

func hmacSHA256(message string, secret []byte) string {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(message))
	sum := mac.Sum(nil)
	return base64.RawURLEncoding.EncodeToString(sum)
}
