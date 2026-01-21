package auth

import (
	"errors"
	"net/http"
	"strings"

	"magitrickle/api/utils"
	"magitrickle/app"
)

const authHeaderPrefix = "Bearer "

func Middleware(app app.Main) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			token := strings.TrimSpace(r.Header.Get("Authorization"))
			if !strings.HasPrefix(token, authHeaderPrefix) {
				utils.WriteError(w, http.StatusUnauthorized, "Unauthorized")
				return
			}
			token = strings.TrimPrefix(token, authHeaderPrefix)
			if token == "" {
				utils.WriteError(w, http.StatusUnauthorized, "Unauthorized")
				return
			}

			login, passwordHash, err := parseTokenSubject(token)
			if err != nil {
				utils.WriteError(w, http.StatusUnauthorized, "Unauthorized")
				return
			}
			if err := verifyToken(token, login, passwordHash); err != nil {
				utils.WriteError(w, http.StatusUnauthorized, "Unauthorized")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

func parseTokenSubject(token string) (string, string, error) {
	claims, err := parseJWTWithoutVerification(token)
	if err != nil {
		return "", "", err
	}
	if claims.Sub == "" {
		return "", "", errors.New("empty subject")
	}
	passwordHash, err := loadPasswordHash(claims.Sub)
	if err != nil {
		return "", "", err
	}
	return claims.Sub, passwordHash, nil
}
