package v1

import (
	"encoding/json"
	"fmt"
	"net/http"

	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
)

func WriteJson(w http.ResponseWriter, httpCode int, data interface{}) {
	buf, err := json.Marshal(data)
	if err != nil {
		WriteError(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(httpCode)
	w.Write(buf)
}

func WriteError(w http.ResponseWriter, httpCode int, e string) {
	WriteJson(w, httpCode, types.ErrorRes{Error: e})
}

func ReadJson[T any](r *http.Request) (T, error) {
	var req T
	err := json.NewDecoder(r.Body).Decode(&req)
	if err != nil {
		err = fmt.Errorf("failed to parse request: %w", err)
	}
	return req, err
}
