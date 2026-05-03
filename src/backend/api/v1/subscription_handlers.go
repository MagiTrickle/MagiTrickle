package v1

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"time"

	"magitrickle/api/utils"
	"magitrickle/api/v1/types"
	"magitrickle/app"
	"magitrickle/models"
	"magitrickle/subscriptions"
	"magitrickle/utils/intID"

	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"
)

// GetSubscriptions
//
//	@Summary		Получить список подписок
//	@Description	Возвращает список подписок
//	@Tags			subscriptions
//	@Produce		json
//	@Success		200	{object}	types.SubscriptionsRes
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions [get]
func (h *Handler) GetSubscriptions(w http.ResponseWriter, r *http.Request) {
	utils.WriteJson(w, http.StatusOK, RespFromSubscriptions(h.app.Subscriptions()))
}

// PutSubscriptions
//
//	@Summary		Обновить список подписок
//	@Description	Обновляет список подписок
//	@Tags			subscriptions
//	@Accept			json
//	@Produce		json
//	@Param			save	query		bool					false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.SubscriptionsReq	true	"Тело запроса"
//	@Success		200		{object}	map[string]string
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions [put]
func (h *Handler) PutSubscriptions(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.SubscriptionsReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Subscriptions == nil {
		utils.WriteError(w, http.StatusBadRequest, "no subscriptions in request")
		return
	}

	existingByID := make(map[intID.ID]*models.Subscription, len(h.app.Subscriptions()))
	for _, sub := range h.app.Subscriptions() {
		existingByID[sub.ID] = sub
	}

	newSubs := make([]*models.Subscription, len(*req.Subscriptions))
	for i, subReq := range *req.Subscriptions {
		if subReq.URL == "" {
			utils.WriteError(w, http.StatusBadRequest, "subscription url is required")
			return
		}
		var existing *models.Subscription
		if subReq.ID != nil {
			existing = existingByID[*subReq.ID]
		}
		sub, err := SubscriptionFromReq(subReq, existing)
		if err != nil {
			utils.WriteError(w, http.StatusBadRequest, err.Error())
			return
		}
		newSubs[i] = sub
	}
	if err := ensureUniqueSubscriptionIDs(newSubs); err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	for _, sub := range newSubs {
		if err := ensureUniqueSubscriptionRuleIDs(sub); err != nil {
			utils.WriteError(w, http.StatusBadRequest, err.Error())
			return
		}
	}

	if err := h.app.ReplaceSubscriptions(newSubs); err != nil {
		utils.WriteError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if r.URL.Query().Get("save") != "false" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
	utils.WriteJson(w, http.StatusOK, map[string]string{"status": "ok"})
}

// CreateSubscription
//
//	@Summary		Создать подписку
//	@Description	Создает подписку
//	@Tags			subscriptions
//	@Accept			json
//	@Produce		json
//	@Param			save	query		bool					false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.SubscriptionReq	true	"Тело запроса"
//	@Success		200		{object}	map[string]string
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions [post]
func (h *Handler) CreateSubscription(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.SubscriptionReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.URL == "" {
		utils.WriteError(w, http.StatusBadRequest, "subscription url is required")
		return
	}
	sub, err := SubscriptionFromReq(req, nil)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := ensureUniqueSubscriptionRuleIDs(sub); err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := h.app.AddSubscription(sub); err != nil {
		status := http.StatusInternalServerError
		if errors.Is(err, app.ErrSubscriptionConflict) {
			status = http.StatusConflict
		}
		utils.WriteError(w, status, err.Error())
		return
	}
	if r.URL.Query().Get("save") != "false" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
	utils.WriteJson(w, http.StatusOK, map[string]string{"status": "ok"})
}

// SyncSubscription
//
//	@Summary		Синхронизировать подписку
//	@Description	Загружает правила по URL и обновляет подписку
//	@Tags			subscriptions
//	@Accept			json
//	@Produce		json
//	@Param			subscriptionID	path		string	true	"ID подписки"
//	@Param			save			query		bool	false	"Сохранить изменения в конфигурационный файл"
//	@Param			json			body		types.SubscriptionSyncReq	false	"Переопределение URL для синхронизации"
//	@Success		200				{object}	map[string]interface{}
//	@Failure		400				{object}	types.ErrorRes
//	@Failure		404				{object}	types.ErrorRes
//	@Failure		502				{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions/{subscriptionID}/sync [post]
func (h *Handler) SyncSubscription(w http.ResponseWriter, r *http.Request) {
	idStr := chi.URLParam(r, "subscriptionID")
	if idStr == "" {
		utils.WriteError(w, http.StatusBadRequest, "subscription id is required")
		return
	}
	id, err := intID.ParseID(idStr)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, "invalid subscription id")
		return
	}

	var req types.SubscriptionSyncReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil && !errors.Is(err, io.EOF) {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}

	target, changed, err := h.app.SyncSubscriptionByID(id, time.Now(), req.URL)
	if err != nil {
		switch {
		case errors.Is(err, app.ErrSubscriptionNotFound):
			utils.WriteError(w, http.StatusNotFound, err.Error())
		case errors.Is(err, app.ErrSubscriptionInvalid):
			utils.WriteError(w, http.StatusBadRequest, err.Error())
		case errors.Is(err, app.ErrSubscriptionFetch):
			utils.WriteError(w, http.StatusBadGateway, err.Error())
		default:
			utils.WriteError(w, http.StatusInternalServerError, err.Error())
		}
		return
	}

	if changed && r.URL.Query().Get("save") != "false" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}

	rulesRes := RespFromSubscriptionRules(target.Rules)
	var rules []types.SubscriptionRuleRes
	if rulesRes.Rules != nil {
		rules = *rulesRes.Rules
	}
	utils.WriteJson(w, http.StatusOK, map[string]interface{}{
		"rules":      rules,
		"lastUpdate": target.LastUpdate,
		"url":        target.URL,
	})
}

// DeleteSubscription
//
//	@Summary		Удалить подписку
//	@Description	Удаляет подписку
//	@Tags			subscriptions
//	@Produce		json
//	@Param			subscriptionID	path		string	true	"ID подписки"
//	@Param			save			query		bool	false	"Сохранить изменения в конфигурационный файл"
//	@Success		200				{object}	map[string]string
//	@Failure		400				{object}	types.ErrorRes
//	@Failure		404				{object}	types.ErrorRes
//	@Failure		500				{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions/{subscriptionID} [delete]
func (h *Handler) DeleteSubscription(w http.ResponseWriter, r *http.Request) {
	idStr := chi.URLParam(r, "subscriptionID")
	if idStr == "" {
		utils.WriteError(w, http.StatusBadRequest, "subscription id is required")
		return
	}
	id, err := intID.ParseID(idStr)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, "invalid subscription id")
		return
	}

	removed, err := h.app.RemoveSubscriptionByID(id)
	if err != nil {
		utils.WriteError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if !removed {
		utils.WriteError(w, http.StatusNotFound, "subscription not found")
		return
	}

	if r.URL.Query().Get("save") != "false" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
	utils.WriteJson(w, http.StatusOK, map[string]string{"status": "ok"})
}

// GetSubscriptionRules
//
//	@Summary		Получить правила подписки по URL
//	@Description	Загружает правила по URL, без сохранения
//	@Tags			subscriptions
//	@Produce		json
//	@Param			url	query		string	true	"URL подписки"
//	@Success		200	{object}	types.SubscriptionRulesRes
//	@Failure		400	{object}	types.ErrorRes
//	@Failure		502	{object}	types.ErrorRes
//	@Router			/api/v1/subscriptions/rules [get]
func (h *Handler) GetSubscriptionRules(w http.ResponseWriter, r *http.Request) {
	url := r.URL.Query().Get("url")
	if url == "" {
		utils.WriteError(w, http.StatusBadRequest, "subscription url is required")
		return
	}
	list, err := subscriptions.FetchList(url)
	if err != nil {
		utils.WriteError(w, http.StatusBadGateway, err.Error())
		return
	}
	rules := subscriptions.ParseRules(list)
	utils.WriteJson(w, http.StatusOK, RespFromSubscriptionRules(rules))
}

func ensureUniqueSubscriptionIDs(subs []*models.Subscription) error {
	dup := make(map[[4]byte]struct{})
	for _, sub := range subs {
		if _, exists := dup[sub.ID]; exists || sub.ID.IsZero() {
			for {
				sub.ID = intID.RandomID()
				if _, exists := dup[sub.ID]; !exists {
					break
				}
			}
		}
		dup[sub.ID] = struct{}{}
	}
	return nil
}

func ensureUniqueSubscriptionRuleIDs(sub *models.Subscription) error {
	dup := make(map[[4]byte]struct{})
	for _, rule := range sub.Rules {
		if _, exists := dup[rule.ID]; exists || rule.ID.IsZero() {
			for {
				rule.ID = intID.RandomID()
				if _, exists := dup[rule.ID]; !exists {
					break
				}
			}
		}
		dup[rule.ID] = struct{}{}
	}
	return nil
}
