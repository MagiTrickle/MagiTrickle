package v1

import (
	"fmt"
	"net/http"
	"strconv"

	"magitrickle/api/types"
	"magitrickle/internal/app"
	"magitrickle/models"

	"github.com/rs/zerolog/log"
)

// Handler предоставляет набор методов для обработки API запросов.
type Handler struct {
	app *app.App
}

// NewHandler создаёт новый обработчик для API v1.
func NewHandler(a *app.App) *Handler {
	return &Handler{app: a}
}

// NetfilterDHook
//
//	@Summary		Хук эвента netfilter.d
//	@Description	Эмитирует хук эвента netfilter.d
//	@Tags			hooks
//	@Accept			json
//	@Produce		json
//	@Param			json	body		types.NetfilterDHookReq	true	"Тело запроса"
//	@Success		200
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/system/hooks/netfilterd [post]
func (h *Handler) NetfilterDHook(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.NetfilterDHookReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	log.Debug().Str("type", req.Type).Str("table", req.Table).Msg("netfilter.d event")
	if h.app.DnsOverrider() != nil {
		if err := h.app.DnsOverrider().NetfilterDHook(req.Type, req.Table); err != nil {
			log.Error().Err(err).Msg("error fixing iptables after netfilter.d")
		}
	}
	for _, group := range h.app.Groups() {
		if err := group.NetfilterDHook(req.Type, req.Table); err != nil {
			log.Error().Err(err).Msg("error while fixing iptables in group")
		}
	}
}

// ListInterfaces
//
//	@Summary		Получить список интерфейсов
//	@Description	Возвращает список интерфейсов
//	@Tags			config
//	@Produce		json
//	@Success		200		{object}	types.InterfacesRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/system/interfaces [get]
func (h *Handler) ListInterfaces(w http.ResponseWriter, r *http.Request) {
	interfaces, err := h.app.ListInterfaces()
	if err != nil {
		WriteError(w, http.StatusInternalServerError, fmt.Errorf("failed to get interfaces: %w", err).Error())
		return
	}
	res := make([]types.InterfaceRes, len(interfaces))
	for i, iface := range interfaces {
		res[i] = types.InterfaceRes{ID: iface.Name}
	}
	WriteJson(w, http.StatusOK, types.InterfacesRes{Interfaces: res})
}

// SaveConfig
//
//	@Summary		Сохранить конфигурацию
//	@Description	Сохраняет текущую конфигурацию в постоянную память
//	@Tags			config
//	@Produce		json
//	@Success		200
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/system/config/save [post]
func (h *Handler) SaveConfig(w http.ResponseWriter, r *http.Request) {
	if err := h.app.SaveConfig(); err != nil {
		WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to save config: %v", err))
	}
}

// GetGroups
//
//	@Summary		Получить список групп
//	@Description	Возвращает список групп
//	@Tags			groups
//	@Produce		json
//	@Param			with_rules	query		bool	false	"Возвращать группы с их правилами"
//	@Success		200			{object}	types.GroupsRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups [get]
func (h *Handler) GetGroups(w http.ResponseWriter, r *http.Request) {
	withRules := r.URL.Query().Get("with_rules") == "true"
	appGroups := h.app.Groups()
	modelGroups := make([]*models.Group, len(appGroups))
	for i, g := range appGroups {
		modelGroups[i] = g.Group
	}
	WriteJson(w, http.StatusOK, ToGroupsRes(modelGroups, withRules))
}

// PutGroups
//
//	@Summary		Обновить список групп
//	@Description	Обновляет список групп
//	@Tags			groups
//	@Accept			json
//	@Produce		json
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.GroupsReq	true	"Тело запроса"
//	@Success		200			{object}	types.GroupsRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups [put]
func (h *Handler) PutGroups(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.GroupsReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Groups == nil {
		WriteError(w, http.StatusBadRequest, "no groups in request")
		return
	}
	for _, g := range h.app.Groups() {
		_ = g.Disable()
	}
	newGroups := make([]*models.Group, len(*req.Groups))
	for i, gReq := range *req.Groups {
		var existing *models.Group
		for _, g := range h.app.Groups() {
			if gReq.ID != nil && g.Group.ID == *gReq.ID {
				existing = g.Group
				break
			}
		}
		newGroups[i], err = FromGroupReq(gReq, existing)
		if err != nil {
			WriteError(w, http.StatusBadRequest, err.Error())
			return
		}
	}
	h.app.ClearGroups()
	for _, grp := range newGroups {
		if err := h.app.AddGroup(grp); err != nil {
			WriteError(w, http.StatusInternalServerError, err.Error())
			return
		}
	}
	WriteJson(w, http.StatusOK, ToGroupsRes(newGroups, true))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// CreateGroup
//
//	@Summary		Создать группу
//	@Description	Создает группу
//	@Tags			groups
//	@Accept			json
//	@Produce		json
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.GroupReq	true	"Тело запроса"
//	@Success		200			{object}	types.GroupRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups [post]
func (h *Handler) CreateGroup(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.GroupReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := FromGroupReq(req, nil)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := h.app.AddGroup(group); err != nil {
		WriteError(w, http.StatusInternalServerError, err.Error())
		return
	}
	WriteJson(w, http.StatusOK, ToGroupRes(group, true))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// GetGroup
//
//	@Summary		Получить группу
//	@Description	Возвращает запрошенную группу
//	@Tags			groups
//	@Produce		json
//	@Param			groupID		path		string	true	"ID группы"
//	@Param			with_rules	query		bool	false	"Возвращать группу с её правилами"
//	@Success		200			{object}	types.GroupRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID} [get]
func (h *Handler) GetGroup(w http.ResponseWriter, r *http.Request) {
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	withRules := r.URL.Query().Get("with_rules") == "true"
	group := h.app.Groups()[groupIdx].Group
	WriteJson(w, http.StatusOK, ToGroupRes(group, withRules))
}

// PutGroup
//
//	@Summary		Обновить группу
//	@Description	Обновляет запрошенную группу
//	@Tags			groups
//	@Accept			json
//	@Produce		json
//	@Param			groupID	path		string			true	"ID группы"
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.GroupReq	true	"Тело запроса"
//	@Success		200			{object}	types.GroupRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID} [put]
func (h *Handler) PutGroup(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.GroupReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]

	enabled := groupWrapper.Enabled()
	if enabled {
		if err := groupWrapper.Disable(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to disable group: %v", err))
			return
		}
	}

	updatedGroup, err := FromGroupReq(req, groupWrapper.Group)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}

	if enabled {
		if err := groupWrapper.Enable(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to enable group: %v", err))
			return
		}
		if err := groupWrapper.Sync(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to sync group: %v", err))
			return
		}
	}
	WriteJson(w, http.StatusOK, ToGroupRes(updatedGroup, true))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// DeleteGroup
//
//	@Summary		Удалить группу
//	@Description	Удаляет запрошенную группу
//	@Tags			groups
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Param			save	query		bool	false	"Сохранить изменения в конфигурационный файл"
//	@Success		200
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID} [delete]
func (h *Handler) DeleteGroup(w http.ResponseWriter, r *http.Request) {
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]
	if groupWrapper.Enabled() {
		if err := groupWrapper.Disable(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to disable group: %v", err))
			return
		}
	}
	h.app.RemoveGroupByIndex(groupIdx)
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// GetRules
//
//	@Summary		Получить список правил
//	@Description	Возвращает список правил
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Success		200			{object}	types.RulesRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [get]
func (h *Handler) GetRules(w http.ResponseWriter, r *http.Request) {
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	rules := h.app.Groups()[groupIdx].Group.Rules
	WriteJson(w, http.StatusOK, ToRulesRes(rules))
}

// PutRules
//
//	@Summary		Обновить список правил
//	@Description	Обновляет список правил
//	@Tags			rules
//	@Accept			json
//	@Produce		json
//	@Param			groupID	path		string			true	"ID группы"
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.RulesReq	true	"Тело запроса"
//	@Success		200			{object}	types.RulesRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [put]
func (h *Handler) PutRules(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.RulesReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Rules == nil {
		WriteError(w, http.StatusBadRequest, "no rules in request")
		return
	}
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]
	enabled := groupWrapper.Enabled()

	newRules := make([]*models.Rule, len(*req.Rules))
	for i, rr := range *req.Rules {
		id := types.RandomID()
		if rr.ID != nil {
			found := false
			for _, oldRule := range groupWrapper.Group.Rules {
				if oldRule.ID == *rr.ID {
					id = *rr.ID
					found = true
					break
				}
			}
			if !found {
				WriteError(w, http.StatusNotFound, "rule not found")
				return
			}
		}
		newRules[i] = &models.Rule{
			ID:     id,
			Name:   rr.Name,
			Type:   rr.Type,
			Rule:   rr.Rule,
			Enable: rr.Enable,
		}
	}
	groupWrapper.Group.Rules = newRules
	if enabled {
		if err := groupWrapper.Sync(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to sync group: %v", err))
			return
		}
	}
	WriteJson(w, http.StatusOK, ToRulesRes(newRules))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// CreateRule
//
//	@Summary		Создать правило
//	@Description	Создает правило
//	@Tags			rules
//	@Accept			json
//	@Produce		json
//	@Param			groupID	path		string			true	"ID группы"
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.RuleReq	true	"Тело запроса"
//	@Success		200			{object}	types.RuleRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [post]
func (h *Handler) CreateRule(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.RuleReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]
	enabled := groupWrapper.Enabled()

	rule, err := FromRuleReq(req, groupWrapper.Group.Rules)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	groupWrapper.Group.Rules = append(groupWrapper.Group.Rules, rule)
	if enabled {
		if err := groupWrapper.Sync(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to sync group: %v", err))
			return
		}
	}
	WriteJson(w, http.StatusOK, ToRuleRes(rule))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// GetRule
//
//	@Summary		Получить правило
//	@Description	Возвращает запрошенное правило
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Param			ruleID	path		string	true	"ID правила"
//	@Success		200			{object}	types.RuleRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [get]
func (h *Handler) GetRule(w http.ResponseWriter, r *http.Request) {
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	ruleIdx, _ := strconv.Atoi(r.Header.Get("ruleIdx"))
	rule := h.app.Groups()[groupIdx].Group.Rules[ruleIdx]
	WriteJson(w, http.StatusOK, ToRuleRes(rule))
}

// PutRule
//
//	@Summary		Обновить правило
//	@Description	Обновляет запрошенное правило
//	@Tags			rules
//	@Accept			json
//	@Produce		json
//	@Param			groupID	path		string			true	"ID группы"
//	@Param			ruleID	path		string			true	"ID правила"
//	@Param			save	query		bool			false	"Сохранить изменения в конфигурационный файл"
//	@Param			json	body		types.RuleReq	true	"Тело запроса"
//	@Success		200			{object}	types.RuleRes
//	@Failure		400			{object}	types.ErrorRes
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [put]
func (h *Handler) PutRule(w http.ResponseWriter, r *http.Request) {
	req, err := ReadJson[types.RuleReq](r)
	if err != nil {
		WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]
	enabled := groupWrapper.Enabled()

	ruleIdx, _ := strconv.Atoi(r.Header.Get("ruleIdx"))
	rule := groupWrapper.Group.Rules[ruleIdx]
	rule.Name = req.Name
	rule.Type = req.Type
	rule.Rule = req.Rule
	rule.Enable = req.Enable

	if enabled {
		if err := groupWrapper.Sync(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to sync group: %v", err))
			return
		}
	}
	WriteJson(w, http.StatusOK, ToRuleRes(rule))
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}

// DeleteRule
//
//	@Summary		Удалить правило
//	@Description	Удаляет запрошенное правило
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Param			ruleID	path		string	true	"ID правила"
//	@Param			save	query		bool	false	"Сохранить изменения в конфигурационный файл"
//	@Success		200
//	@Failure		404			{object}	types.ErrorRes
//	@Failure		500			{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [delete]
func (h *Handler) DeleteRule(w http.ResponseWriter, r *http.Request) {
	groupIdx, _ := strconv.Atoi(r.Header.Get("groupIdx"))
	groupWrapper := h.app.Groups()[groupIdx]
	enabled := groupWrapper.Enabled()

	ruleIdx, _ := strconv.Atoi(r.Header.Get("ruleIdx"))
	groupWrapper.Group.Rules = append(groupWrapper.Group.Rules[:ruleIdx], groupWrapper.Group.Rules[ruleIdx+1:]...)
	if enabled {
		if err := groupWrapper.Sync(); err != nil {
			WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to sync group: %v", err))
			return
		}
	}
	if r.URL.Query().Get("save") == "true" {
		if err := h.app.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
}
