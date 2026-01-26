package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"time"
)

const (
	jwtIssuer = "magitrickle"
	jwtYears  = 20
)

func Authenticate(login, password string) (string, error) {
	if login == "" || password == "" {
		return "", errors.New("missing credentials")
	}
	passwordHash, err := loadPasswordHash(login)
	if err != nil {
		return "", err
	}
	if !verifyPassword(password, passwordHash) {
		return "", errors.New("invalid credentials")
	}
	return issueToken(login, passwordHash)
}

func verifyPassword(password, passwordHash string) bool {
	computed, err := cryptPassword(password, passwordHash)
	if err != nil {
		return false
	}
	return computed == passwordHash
}

func issueToken(login, passwordHash string) (string, error) {
	secret, err := LoadAppSecret()
	if err != nil {
		return "", err
	}
	signingKey := deriveSigningKey(secret, passwordHash)

	issuedAt := time.Now().UTC()
	expiresAt := issuedAt.AddDate(jwtYears, 0, 0)

	claims := jwtClaims{
		Sub: login,
		Iss: jwtIssuer,
		Iat: issuedAt.Unix(),
		Exp: expiresAt.Unix(),
	}

	header := jwtHeader{Alg: "HS256", Typ: "JWT"}
	return signJWT(header, claims, signingKey)
}

func verifyToken(token, login, passwordHash string) error {
	secret, err := LoadAppSecret()
	if err != nil {
		return err
	}
	key := deriveSigningKey(secret, passwordHash)
	claims, err := parseAndVerifyJWT(token, key)
	if err != nil {
		return err
	}
	if claims.Sub != login {
		return errors.New("token subject mismatch")
	}
	if claims.Iss != jwtIssuer {
		return errors.New("token issuer mismatch")
	}
	if claims.Exp <= time.Now().UTC().Unix() {
		return errors.New("token expired")
	}
	return nil
}

func deriveSigningKey(secret []byte, passwordHash string) []byte {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(passwordHash))
	return []byte(hex.EncodeToString(mac.Sum(nil)))
}
