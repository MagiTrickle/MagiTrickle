package intID

import "slices"

// EnsureUnique выдаёт новые ID элементам с пустым или повторяющимся ID.
// Элементы могут быть общими с опубликованными моделями, поэтому
// исправленный элемент заменяется результатом withID в копии среза, а
// исходные элементы и срез не меняются. fixed — сколько элементов заменено.
func EnsureUnique[T any](items []*T, id func(*T) ID, withID func(*T, ID) *T) (result []*T, fixed int) {
	used := make(map[ID]struct{}, len(items))
	for _, item := range items {
		used[id(item)] = struct{}{}
	}

	seen := make(map[ID]struct{}, len(items))
	result = items
	for i, item := range items {
		current := id(item)
		if _, dup := seen[current]; !dup && !current.IsZero() {
			seen[current] = struct{}{}
			continue
		}

		next := RandomID()
		for {
			_, taken := used[next]
			if !taken && !next.IsZero() {
				break
			}
			next = RandomID()
		}
		used[next], seen[next] = struct{}{}, struct{}{}

		if fixed == 0 {
			result = slices.Clone(items)
		}
		result[i] = withID(item, next)
		fixed++
	}
	return result, fixed
}
