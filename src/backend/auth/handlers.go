package auth

import (
	"net/http"

	"magitrickle/api/utils"
	"magitrickle/app"
)

type LoginRequest struct {
	Login    string `json:"login"`
	Password string `json:"password"`
}

type LoginResponse struct {
	Token string `json:"token"`
}

func LoginHandler(app app.Main) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !app.Config().HTTPWeb.Auth.Enabled {
			utils.WriteError(w, http.StatusNotFound, "Auth disabled")
			return
		}

		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		if r.Method != http.MethodPost {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		req, err := utils.ReadJson[LoginRequest](r)
		if err != nil {
			utils.WriteError(w, http.StatusBadRequest, err.Error())
			return
		}
		if req.Login == "" || req.Password == "" {
			utils.WriteError(w, http.StatusBadRequest, "missing credentials")
			return
		}

		token, err := Authenticate(req.Login, req.Password)
		if err != nil {
			utils.WriteError(w, http.StatusForbidden, "Invalid credentials")
			return
		}
		utils.WriteJson(w, http.StatusOK, LoginResponse{Token: token})
	}
}
