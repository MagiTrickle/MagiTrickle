package v1

import (
	"errors"
	"fmt"
	"net/http"
	"slices"

	"magitrickle/api/utils"
	"magitrickle/api/v1/types"
	"magitrickle/app"
	"magitrickle/models"
	"magitrickle/utils/intID"

	"github.com/rs/zerolog/log"
)

// Handler предоставляет набор методов для обработки API запросов.
type Handler struct {
	app app.Main
}

// NewHandler создаёт новый обработчик для API v1.
func NewHandler(a app.Main) *Handler {
	return &Handler{app: a}
}

var errRuleNotFound = errors.New("rule not found")

// badRequestError — ошибка в самом запросе, обнаруженная внутри UpdateGroup.
type badRequestError struct{ error }

func (e badRequestError) Unwrap() error { return e.error }

// writeGroupError отвечает ошибкой изменения групп.
func writeGroupError(w http.ResponseWriter, err error) {
	var badRequest badRequestError
	switch {
	case errors.As(err, &badRequest):
		utils.WriteError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, app.ErrGroupNotFound), errors.Is(err, errRuleNotFound):
		utils.WriteError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, app.ErrGroupIDConflict), errors.Is(err, app.ErrRuleIDConflict):
		utils.WriteError(w, http.StatusConflict, err.Error())
	default:
		utils.WriteError(w, http.StatusInternalServerError, err.Error())
	}
}

// save сохраняет конфиг, если клиент об этом просил (?save=true, а при
// saveByDefault — если не ?save=false). Вызывается до ответа: изменение к
// этому моменту уже применено, но если сохранить не удалось, клиент должен
// получить ошибку, а не считать изменение сохранённым. false — ответ с
// ошибкой уже отправлен.
func (h *Handler) save(w http.ResponseWriter, r *http.Request, saveByDefault bool) bool {
	switch r.URL.Query().Get("save") {
	case "true":
	case "false":
		return true
	default:
		if !saveByDefault {
			return true
		}
	}
	if err := h.app.SaveConfig(); err != nil {
		utils.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("changes are applied but not saved: %v", err))
		return false
	}
	return true
}

// NetfilterDHook
//
//	@Summary		Хук эвента netfilter.d
//	@Description	Эмитирует хук эвента netfilter.d
//	@Tags			hooks
//	@Accept			json
//	@Produce		json
//	@Param			json	body	types.NetfilterDHookReq	true	"Тело запроса"
//	@Success		200
//	@Failure		400	{object}	types.ErrorRes
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/system/hooks/netfilterd [post]
func (h *Handler) NetfilterDHook(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.NetfilterDHookReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	log.Debug().
		Str("type", req.Type).
		Str("table", req.Table).
		Msg("received netfilter.d event")
	err = h.app.ForceCommitIPTables()
	if err != nil {
		log.Error().Err(err).Msg("error fixing iptables after netfilter.d")
	}
}

// ListInterfaces
//
//	@Summary		Получить список интерфейсов
//	@Description	Возвращает список интерфейсов
//	@Tags			config
//	@Produce		json
//	@Success		200	{object}	types.InterfacesRes
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/system/interfaces [get]
func (h *Handler) ListInterfaces(w http.ResponseWriter, r *http.Request) {
	interfaces, err := h.app.ListInterfaces()
	if err != nil {
		utils.WriteError(w, http.StatusInternalServerError, fmt.Errorf("failed to get interfaces: %w", err).Error())
		return
	}

	res := make([]types.InterfaceRes, len(interfaces)+1)
	res[0] = types.InterfaceRes{ID: "blackhole"}
	for i, iface := range interfaces {
		res[i+1] = types.InterfaceRes{
			ID:   iface.ID,
			Name: iface.Name,
		}
	}

	utils.WriteJson(w, http.StatusOK, types.InterfacesRes{Interfaces: res})
}

// SaveConfig
//
//	@Summary		Сохранить конфигурацию
//	@Description	Сохраняет текущую конфигурацию в постоянную память
//	@Tags			config
//	@Produce		json
//	@Success		200
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/system/config/save [post]
func (h *Handler) SaveConfig(w http.ResponseWriter, r *http.Request) {
	if err := h.app.SaveConfig(); err != nil {
		utils.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to save config: %v", err))
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
	utils.WriteJson(w, http.StatusOK, RespFromGroups(h.app.Groups(), withRules))
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
//	@Success		200		{object}	types.GroupsRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups [put]
func (h *Handler) PutGroups(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.GroupsReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Groups == nil {
		utils.WriteError(w, http.StatusBadRequest, "no groups in request")
		return
	}
	// Новый список строится по снимку без блокировки: это полная замена, и
	// правки, пришедшие параллельно с ней, перезаписываются.
	existing := make(map[intID.ID]*models.Group)
	for _, g := range h.app.Groups() {
		existing[g.ID] = g
	}
	newGroups := make([]*models.Group, len(*req.Groups))
	for i, gReq := range *req.Groups {
		var old *models.Group
		if gReq.ID != nil {
			old = existing[*gReq.ID]
		}
		newGroups[i], err = GroupFromReq(gReq, old)
		if err != nil {
			utils.WriteError(w, http.StatusBadRequest, err.Error())
			return
		}
	}
	if err := h.app.ReplaceGroups(newGroups); err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromGroups(newGroups, true))
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
//	@Success		200		{object}	types.GroupRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups [post]
func (h *Handler) CreateGroup(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.GroupReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := GroupFromReq(req, nil)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := h.app.AddGroup(group); err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromGroup(group, true))
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
	withRules := r.URL.Query().Get("with_rules") == "true"
	utils.WriteJson(w, http.StatusOK, RespFromGroup(groupFromCtx(r), withRules))
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
//	@Success		200		{object}	types.GroupRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID} [put]
func (h *Handler) PutGroup(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.GroupReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := h.app.UpdateGroup(groupFromCtx(r).ID, func(current *models.Group) (*models.Group, error) {
		group, err := GroupFromReq(req, current)
		if err != nil {
			return nil, badRequestError{err}
		}
		return group, nil
	})
	if err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromGroup(group, true))
}

// DeleteGroup
//
//	@Summary		Удалить группу
//	@Description	Удаляет запрошенную группу
//	@Tags			groups
//	@Produce		json
//	@Param			groupID	path	string	true	"ID группы"
//	@Param			save	query	bool	false	"Сохранить изменения в конфигурационный файл"
//	@Success		200
//	@Failure		404	{object}	types.ErrorRes
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID} [delete]
func (h *Handler) DeleteGroup(w http.ResponseWriter, r *http.Request) {
	removed, err := h.app.RemoveGroup(groupFromCtx(r).ID)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	if !removed {
		// Группу удалили параллельно, пока шёл запрос.
		writeGroupError(w, app.ErrGroupNotFound)
		return
	}
	h.save(w, r, false)
}

// GetRules
//
//	@Summary		Получить список правил
//	@Description	Возвращает список правил
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Success		200		{object}	types.RulesRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [get]
func (h *Handler) GetRules(w http.ResponseWriter, r *http.Request) {
	utils.WriteJson(w, http.StatusOK, RespFromRules(groupFromCtx(r).Rules))
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
//	@Success		200		{object}	types.RulesRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [put]
func (h *Handler) PutRules(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.RulesReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Rules == nil {
		utils.WriteError(w, http.StatusBadRequest, "no rules in request")
		return
	}
	group, err := h.app.UpdateGroup(groupFromCtx(r).ID, func(current *models.Group) (*models.Group, error) {
		group := current.Clone()
		group.Rules = RulesFromReq(*req.Rules, current.Rules)
		return group, nil
	})
	if err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromRules(group.Rules))
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
//	@Success		200		{object}	types.RuleRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules [post]
func (h *Handler) CreateRule(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.RuleReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	rule := RuleFromReq(req)
	_, err = h.app.UpdateGroup(groupFromCtx(r).ID, func(current *models.Group) (*models.Group, error) {
		group := current.Clone()
		group.Rules = append(group.Rules, rule)
		return group, nil
	})
	if err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromRule(rule))
}

// GetRule
//
//	@Summary		Получить правило
//	@Description	Возвращает запрошенное правило
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path		string	true	"ID группы"
//	@Param			ruleID	path		string	true	"ID правила"
//	@Success		200		{object}	types.RuleRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [get]
func (h *Handler) GetRule(w http.ResponseWriter, r *http.Request) {
	group := groupFromCtx(r)
	utils.WriteJson(w, http.StatusOK, RespFromRule(group.Rules[ruleIndex(group, ruleIDFromCtx(r))]))
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
//	@Success		200		{object}	types.RuleRes
//	@Failure		400		{object}	types.ErrorRes
//	@Failure		404		{object}	types.ErrorRes
//	@Failure		409		{object}	types.ErrorRes
//	@Failure		500		{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [put]
func (h *Handler) PutRule(w http.ResponseWriter, r *http.Request) {
	req, err := utils.ReadJson[types.RuleReq](r)
	if err != nil {
		utils.WriteError(w, http.StatusBadRequest, err.Error())
		return
	}
	id := ruleIDFromCtx(r)
	if req.ID != nil && !req.ID.IsZero() && *req.ID != id {
		utils.WriteError(w, http.StatusBadRequest, "rule ID mismatch")
		return
	}
	req.ID = &id
	rule := RuleFromReq(req)

	_, err = h.app.UpdateGroup(groupFromCtx(r).ID, func(current *models.Group) (*models.Group, error) {
		idx := ruleIndex(current, id)
		if idx < 0 {
			return nil, errRuleNotFound
		}
		group := current.Clone()
		group.Rules[idx] = rule
		return group, nil
	})
	if err != nil {
		writeGroupError(w, err)
		return
	}
	if !h.save(w, r, false) {
		return
	}
	utils.WriteJson(w, http.StatusOK, RespFromRule(rule))
}

// DeleteRule
//
//	@Summary		Удалить правило
//	@Description	Удаляет запрошенное правило
//	@Tags			rules
//	@Produce		json
//	@Param			groupID	path	string	true	"ID группы"
//	@Param			ruleID	path	string	true	"ID правила"
//	@Param			save	query	bool	false	"Сохранить изменения в конфигурационный файл"
//	@Success		200
//	@Failure		404	{object}	types.ErrorRes
//	@Failure		500	{object}	types.ErrorRes
//	@Router			/api/v1/groups/{groupID}/rules/{ruleID} [delete]
func (h *Handler) DeleteRule(w http.ResponseWriter, r *http.Request) {
	id := ruleIDFromCtx(r)
	_, err := h.app.UpdateGroup(groupFromCtx(r).ID, func(current *models.Group) (*models.Group, error) {
		idx := ruleIndex(current, id)
		if idx < 0 {
			return nil, errRuleNotFound
		}
		group := current.Clone()
		group.Rules = slices.Delete(group.Rules, idx, idx+1)
		return group, nil
	})
	if err != nil {
		writeGroupError(w, err)
		return
	}
	h.save(w, r, false)
}
