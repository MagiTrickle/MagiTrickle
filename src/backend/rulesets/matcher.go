package rulesets

import (
	"maps"
	"slices"
	"strings"

	"magitrickle/models"
)

// Matcher проверяет доменное имя по доменным правилам набора.
//
// Правила domain, namespace и wildcard вида "*<суффикс>" разложены по
// хэш-множествам, и проверка имени стоит O(длины имени) независимо от
// числа правил — в подписках их бывают десятки тысяч. Все прочие правила
// проверяются по очереди через models.Rule.IsMatch.
type Matcher struct {
	domains    map[string]struct{} // domain: точное совпадение
	namespaces map[string]struct{} // namespace: сам домен и все его поддомены
	suffixes   map[string]struct{} // wildcard "*<суффикс>": любое имя с этим окончанием
	patterns   []*models.Rule      // всё, что не ускоряется индексом
}

// Match сообщает, подпадает ли имя хотя бы под одно правило.
func (m *Matcher) Match(name string) bool {
	if m == nil {
		return false
	}
	if _, ok := m.domains[name]; ok {
		return true
	}

	if len(m.namespaces) > 0 {
		for zone := name; ; {
			if _, ok := m.namespaces[zone]; ok {
				return true
			}
			dot := strings.IndexByte(zone, '.')
			if dot < 0 {
				break
			}
			zone = zone[dot+1:]
		}
	}

	if len(m.suffixes) > 0 {
		for i := 0; i <= len(name); i++ {
			if _, ok := m.suffixes[name[i:]]; ok {
				return true
			}
		}
	}

	for _, rule := range m.patterns {
		if rule.IsMatch(name) {
			return true
		}
	}
	return false
}

// Equal сообщает, что два Matcher заведомо совпадают на любом имени.
// Сравнивается собранный индекс, а не исходные правила: порядок и дубли
// domain/namespace/wildcard-суффиксов не важны. false не означает, что
// поведение различается, — только что это не удалось доказать.
func (m *Matcher) Equal(other *Matcher) bool {
	if m == other {
		return true
	}
	if m == nil || other == nil {
		return m.empty() && other.empty()
	}
	return maps.Equal(m.domains, other.domains) &&
		maps.Equal(m.namespaces, other.namespaces) &&
		maps.Equal(m.suffixes, other.suffixes) &&
		slices.EqualFunc(m.patterns, other.patterns, func(a, b *models.Rule) bool {
			return a.Type == b.Type && a.Rule == b.Rule
		})
}

func (m *Matcher) empty() bool {
	return m == nil || len(m.domains)+len(m.namespaces)+len(m.suffixes)+len(m.patterns) == 0
}

// add добавляет правило в индекс, а если индекс его не ускоряет — в
// patterns, где оно проверяется через models.Rule.IsMatch.
func (m *Matcher) add(ruleType, rule string) {
	switch ruleType {
	case models.RuleTypeDomain:
		put(&m.domains, rule)
		return
	case models.RuleTypeNamespace:
		put(&m.namespaces, rule)
		return
	case models.RuleTypeWildcard:
		if suffix, ok := strings.CutPrefix(rule, "*"); ok && !strings.ContainsAny(suffix, "*?") {
			put(&m.suffixes, suffix)
			return
		}
	}
	m.patterns = append(m.patterns, &models.Rule{Type: ruleType, Rule: rule, Enable: true})
}

func put(set *map[string]struct{}, key string) {
	if *set == nil {
		*set = make(map[string]struct{})
	}
	(*set)[key] = struct{}{}
}
