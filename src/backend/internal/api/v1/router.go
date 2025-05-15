package v1

import (
	"net/http"
	"strconv"

	"magitrickle/api/types"

	"github.com/go-chi/chi/v5"
)

// NewRouter собирает маршруты API v1
func NewRouter(h *Handler) chi.Router {
	r := chi.NewRouter()

	r.Route("/v1", func(r chi.Router) {
		r.Route("/groups", func(r chi.Router) {
			r.Get("/", h.GetGroups)
			r.Put("/", h.PutGroups)
			r.Post("/", h.CreateGroup)
			r.Route("/{groupID}", func(r chi.Router) {
				r.Use(func(next http.Handler) http.Handler {
					return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
						groupID := chi.URLParam(r, "groupID")
						id, err := types.ParseID(groupID)
						if err != nil {
							WriteError(w, http.StatusBadRequest, "invalid group id")
							return
						}
						for i, group := range h.app.Groups() {
							if group.ID == id {
								r.Header.Set("groupIdx", strconv.Itoa(i))
								next.ServeHTTP(w, r)
								return
							}
						}
						WriteError(w, http.StatusNotFound, "group not exist")
					})
				})
				r.Get("/", h.GetGroup)
				r.Put("/", h.PutGroup)
				r.Delete("/", h.DeleteGroup)
				r.Route("/rules", func(r chi.Router) {
					r.Get("/", h.GetRules)
					r.Put("/", h.PutRules)
					r.Post("/", h.CreateRule)
					r.Route("/{ruleID}", func(r chi.Router) {
						r.Use(func(next http.Handler) http.Handler {
							return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
								ruleID := chi.URLParam(r, "ruleID")
								id, err := types.ParseID(ruleID)
								if err != nil {
									WriteError(w, http.StatusBadRequest, "invalid rule id")
									return
								}
								groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
								for idx, rule := range h.app.Groups()[groupIdx].Rules {
									if rule.ID == id {
										r.Header.Set("ruleIdx", strconv.Itoa(idx))
										next.ServeHTTP(w, r)
										return
									}
								}
								WriteError(w, http.StatusNotFound, "rule not exist")
							})
						})
						r.Get("/", h.GetRule)
						r.Put("/", h.PutRule)
						r.Delete("/", h.DeleteRule)
					})
				})
			})
		})
		r.Route("/system", func(r chi.Router) {
			r.Get("/interfaces", h.ListInterfaces)
			r.Route("/config", func(r chi.Router) {
				r.Post("/save", h.SaveConfig)
			})
			r.Route("/hooks", func(r chi.Router) {
				r.Post("/netfilterd", h.NetfilterDHook)
			})
		})
		r.Get("/logs", h.LogsHandler)
	})
	return r
}
