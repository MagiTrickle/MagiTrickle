package v1

import (
	"context"
	"net/http"
	"slices"

	"magitrickle/api/auth"
	"magitrickle/api/utils"
	"magitrickle/app"
	"magitrickle/models"
	"magitrickle/utils/intID"

	"github.com/go-chi/chi/v5"
)

type ctxKey int

const (
	groupCtxKey ctxKey = iota
	ruleIDCtxKey
)

// NewRouter собирает маршруты API v1
func NewRouter(a app.Main) chi.Router {
	h := NewHandler(a)
	r := chi.NewRouter()
	r.Get("/auth", auth.StatusHandler(a))
	r.Post("/auth", auth.LoginHandler(a))
	r.Route("/groups", func(r chi.Router) {
		r.Get("/", h.GetGroups)
		r.Put("/", h.PutGroups)
		r.Post("/", h.CreateGroup)
		r.Route("/{groupID}", func(r chi.Router) {
			r.Use(h.groupCtx)
			r.Get("/", h.GetGroup)
			r.Put("/", h.PutGroup)
			r.Delete("/", h.DeleteGroup)
			r.Route("/rules", func(r chi.Router) {
				r.Get("/", h.GetRules)
				r.Put("/", h.PutRules)
				r.Post("/", h.CreateRule)
				r.Route("/{ruleID}", func(r chi.Router) {
					r.Use(h.ruleCtx)
					r.Get("/", h.GetRule)
					r.Put("/", h.PutRule)
					r.Delete("/", h.DeleteRule)
				})
			})
		})
	})
	r.Route("/subscriptions", func(r chi.Router) {
		r.Get("/", h.GetSubscriptions)
		r.Put("/", h.PutSubscriptions)
		r.Post("/", h.CreateSubscription)
		r.Get("/rules", h.GetSubscriptionRules)
		r.Route("/{subscriptionID}", func(r chi.Router) {
			r.Post("/sync", h.SyncSubscription)
			r.Delete("/", h.DeleteSubscription)
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
	return r
}

// groupCtx кладёт в контекст запроса снимок группы из URL.
func (h *Handler) groupCtx(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id, err := intID.ParseID(chi.URLParam(r, "groupID"))
		if err != nil {
			utils.WriteError(w, http.StatusBadRequest, "invalid group id")
			return
		}
		group := h.app.Group(id)
		if group == nil {
			utils.WriteError(w, http.StatusNotFound, "group not exist")
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), groupCtxKey, group)))
	})
}

// ruleCtx кладёт в контекст запроса ID правила из URL, убедившись, что
// такое правило есть в снимке группы. Изменяющие хэндлеры ищут правило
// заново внутри UpdateGroup: снимок к тому моменту мог устареть.
func (h *Handler) ruleCtx(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id, err := intID.ParseID(chi.URLParam(r, "ruleID"))
		if err != nil {
			utils.WriteError(w, http.StatusBadRequest, "invalid rule id")
			return
		}
		if ruleIndex(groupFromCtx(r), id) < 0 {
			utils.WriteError(w, http.StatusNotFound, "rule not exist")
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), ruleIDCtxKey, id)))
	})
}

// groupFromCtx возвращает снимок группы на момент начала запроса. Для
// изменений используйте UpdateGroup — он передаёт актуальную версию.
func groupFromCtx(r *http.Request) *models.Group {
	return r.Context().Value(groupCtxKey).(*models.Group)
}

func ruleIDFromCtx(r *http.Request) intID.ID {
	return r.Context().Value(ruleIDCtxKey).(intID.ID)
}

func ruleIndex(group *models.Group, id intID.ID) int {
	return slices.IndexFunc(group.Rules, func(rule *models.Rule) bool { return rule.ID == id })
}
