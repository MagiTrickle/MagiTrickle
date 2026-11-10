// Package rulesets приводит подписки и пользовательские группы к одному виду —
// набору правил, по которому трафик направляется в интерфейс.
//
// Пакет ничего не знает о том, как маршрутизация устроена внизу; он только
// решает, какие наборы активны и в каком порядке они применяются.
package rulesets

import (
	"net/netip"
	"slices"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

// RuleSet — снимок набора правил, уже разобранный для применения:
// доменные правила собраны в Matcher, подсети разобраны в префиксы.
// Выключенные правила в набор не попадают, а изменения моделей после
// сборки на него не влияют.
type RuleSet struct {
	ID        intID.ID
	Name      string
	Interface string
	Domains   *Matcher       // все правила, кроме подсетей
	Subnets   []netip.Prefix // правила subnet и subnet6

	// hosts — одиночные адреса из Subnets: они бессрочны, и временная
	// запись из DNS не должна их вытеснять.
	hosts map[netip.Addr]struct{}
}

// PermanentHost сообщает, есть ли в наборе subnet-правило ровно на этот адрес.
func (rs RuleSet) PermanentHost(addr netip.Addr) bool {
	_, ok := rs.hosts[addr]
	return ok
}

// Key — имя маршрута набора. ID групп и подписок не пересекаются, поэтому
// ключ однозначен среди всех наборов.
func (rs RuleSet) Key() string {
	return rs.ID.String()
}

// Matches сообщает, подпадает ли хотя бы одно из имён под доменные правила.
func (rs RuleSet) Matches(names []string) bool {
	for _, name := range names {
		if rs.Domains.Match(name) {
			return true
		}
	}
	return false
}

// SameRules сообщает, что два набора заведомо наполняют маршрут одинаково
// (см. Matcher.Equal). По нему решается, можно ли не ресинкать маршрут,
// поэтому ложное false стоит лишнего ресинка, а ложного true быть не должно.
func (rs RuleSet) SameRules(other RuleSet) bool {
	return rs.Domains.Equal(other.Domains) && sameSubnets(rs.Subnets, other.Subnets)
}

func sameSubnets(a, b []netip.Prefix) bool {
	if len(a) == len(b) && (len(a) == 0 || &a[0] == &b[0]) {
		return true
	}
	return slices.Equal(a, b)
}

func (rs *RuleSet) add(ruleType, rule string) {
	switch ruleType {
	case models.RuleTypeSubnet:
		if prefix, ok := parsePrefix(rule); ok && prefix.Addr().Is4() {
			rs.addSubnet(prefix)
		}
	case models.RuleTypeSubnet6:
		if prefix, ok := parsePrefix(rule); ok && prefix.Addr().Is6() {
			rs.addSubnet(prefix)
		}
	default:
		rs.Domains.add(ruleType, rule)
	}
}

func (rs *RuleSet) addSubnet(prefix netip.Prefix) {
	rs.Subnets = append(rs.Subnets, prefix)
	if prefix.IsSingleIP() {
		if rs.hosts == nil {
			rs.hosts = make(map[netip.Addr]struct{})
		}
		rs.hosts[prefix.Addr()] = struct{}{}
	}
}

// parsePrefix разбирает подсеть вида "addr/bits" или одиночный адрес "addr".
func parsePrefix(rule string) (netip.Prefix, bool) {
	if prefix, err := netip.ParsePrefix(rule); err == nil {
		return prefix.Masked(), true
	}
	if addr, err := netip.ParseAddr(rule); err == nil {
		return netip.PrefixFrom(addr, addr.BitLen()), true
	}
	return netip.Prefix{}, false
}

// Build собирает активные наборы правил в порядке их применения.
//
// Наборы применяются последовательно, и при пересечении побеждает последний.
// Поэтому сначала идут подписки, а за ними пользовательские группы: явно
// заданное пользователем важнее подписки, а среди групп действует их порядок.
func Build(subscriptions []*models.Subscription, groups []*models.Group) []RuleSet {
	return new(Builder).Build(subscriptions, groups)
}

// Builder собирает наборы правил так же, как Build, но помнит собранные в
// прошлый раз, и заново разбирает правила только изменившихся моделей.
// Модели после публикации не меняются, а их Clone разделяет с оригиналом
// объекты правил. Поэтому если у модели с тем же ID тот же список правил
// (те же объекты в том же порядке), набор берётся готовым — даже если сама
// модель заменена ради другого поля, например времени проверки подписки.
// SameRules с таким набором сводится к сравнению указателей. Одновременно
// Build вызывать нельзя.
type Builder struct {
	subscriptions map[intID.ID]built[models.SubscriptionRule]
	groups        map[intID.ID]built[models.Rule]
}

// built — собранный набор и правила, из которых он собран.
type built[R any] struct {
	rules   []*R
	ruleSet RuleSet
}

func (b *Builder) Build(subscriptions []*models.Subscription, groups []*models.Group) []RuleSet {
	list := make([]RuleSet, 0, len(subscriptions)+len(groups))

	nextSubscriptions := make(map[intID.ID]built[models.SubscriptionRule], len(subscriptions))
	for _, sub := range subscriptions {
		if !subscriptionActive(sub) {
			continue
		}
		prev, cached := b.subscriptions[sub.ID]
		rs := cachedOrBuild(prev, cached, sub.Rules, sub.Name, sub.Interface, func() RuleSet {
			rs, _ := FromSubscription(sub)
			return rs
		})
		nextSubscriptions[sub.ID] = built[models.SubscriptionRule]{rules: sub.Rules, ruleSet: rs}
		list = append(list, rs)
	}

	nextGroups := make(map[intID.ID]built[models.Rule], len(groups))
	for _, group := range groups {
		if !groupActive(group) {
			continue
		}
		prev, cached := b.groups[group.ID]
		rs := cachedOrBuild(prev, cached, group.Rules, group.Name, group.Interface, func() RuleSet {
			rs, _ := FromGroup(group)
			return rs
		})
		nextGroups[group.ID] = built[models.Rule]{rules: group.Rules, ruleSet: rs}
		list = append(list, rs)
	}

	b.subscriptions, b.groups = nextSubscriptions, nextGroups
	return list
}

// cachedOrBuild возвращает прежний набор с обновлёнными именем и
// интерфейсом, если правила те же объекты в том же порядке, иначе собирает
// набор заново.
func cachedOrBuild[R any](prev built[R], cached bool, rules []*R, name, iface string, build func() RuleSet) RuleSet {
	if !cached || !slices.Equal(prev.rules, rules) {
		return build()
	}
	rs := prev.ruleSet
	rs.Name, rs.Interface = name, iface
	return rs
}

func subscriptionActive(sub *models.Subscription) bool {
	return sub != nil && sub.Enable && sub.Interface != ""
}

func groupActive(group *models.Group) bool {
	return group != nil && group.Enable && group.Interface != ""
}

// FromSubscription строит набор правил подписки. ok == false, если подписка
// выключена или ей не назначен интерфейс.
func FromSubscription(sub *models.Subscription) (rs RuleSet, ok bool) {
	if !subscriptionActive(sub) {
		return RuleSet{}, false
	}

	rs = RuleSet{ID: sub.ID, Name: sub.Name, Interface: sub.Interface, Domains: new(Matcher)}
	for _, rule := range sub.Rules {
		if rule.Enable {
			rs.add(rule.Type, rule.Rule)
		}
	}
	return rs, true
}

// FromGroup строит набор правил группы. ok == false, если группа выключена
// или ей не назначен интерфейс.
func FromGroup(group *models.Group) (rs RuleSet, ok bool) {
	if !groupActive(group) {
		return RuleSet{}, false
	}

	rs = RuleSet{ID: group.ID, Name: group.Name, Interface: group.Interface, Domains: new(Matcher)}
	for _, rule := range group.Rules {
		if rule.Enable {
			rs.add(rule.Type, rule.Rule)
		}
	}
	return rs, true
}
