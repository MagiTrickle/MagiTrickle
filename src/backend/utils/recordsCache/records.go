package recordsCache

import (
	"bytes"
	"context"
	"net"
	"sync"
	"time"
)

type Address struct {
	Address  net.IP
	Deadline time.Time
}

type Alias struct {
	Alias    string
	Deadline time.Time
}

type Records struct {
	locker sync.RWMutex

	// Раздельные map'ы вместо map[string]interface{}
	addresses map[string][]*Address
	aliases   map[string]*Alias

	// Обратный индекс: alias → []domains, которые на него ссылаются
	reverseAliases map[string][]string
}

func (r *Records) AddAlias(domainName, alias string, ttl uint32) {
	if domainName == alias {
		return
	}

	r.locker.Lock()
	defer r.locker.Unlock()

	deadline := time.Now().Add(time.Duration(ttl) * time.Second)

	// Удаляем старый reverse alias если был
	if oldAlias, ok := r.aliases[domainName]; ok {
		r.removeReverseAlias(oldAlias.Alias, domainName)
	}

	r.aliases[domainName] = &Alias{
		Alias:    alias,
		Deadline: deadline,
	}

	// Добавляем reverse alias
	r.reverseAliases[alias] = append(r.reverseAliases[alias], domainName)
}

func (r *Records) removeReverseAlias(alias, domainName string) {
	domains := r.reverseAliases[alias]
	for i, d := range domains {
		if d == domainName {
			// Удаляем элемент без сохранения порядка
			domains[i] = domains[len(domains)-1]
			r.reverseAliases[alias] = domains[:len(domains)-1]
			break
		}
	}
	if len(r.reverseAliases[alias]) == 0 {
		delete(r.reverseAliases, alias)
	}
}

func (r *Records) AddAddress(domainName string, addr net.IP, ttl uint32) {
	r.locker.Lock()
	defer r.locker.Unlock()

	deadline := time.Now().Add(time.Duration(ttl) * time.Second)

	addresses := r.addresses[domainName]
	for _, aRecord := range addresses {
		if bytes.Equal(aRecord.Address, addr) {
			aRecord.Deadline = deadline
			return
		}
	}

	r.addresses[domainName] = append(addresses, &Address{
		Address:  addr,
		Deadline: deadline,
	})
}

// GetAliases возвращает все домены, которые ссылаются на данный (прямо или транзитивно)
func (r *Records) GetAliases(domainName string) []string {
	r.locker.RLock()
	defer r.locker.RUnlock()

	result := []string{domainName}
	queue := []string{domainName}
	seen := make(map[string]struct{})
	seen[domainName] = struct{}{}

	for len(queue) > 0 {
		current := queue[0]
		queue = queue[1:]

		for _, pointing := range r.reverseAliases[current] {
			if _, ok := seen[pointing]; ok {
				continue
			}
			seen[pointing] = struct{}{}
			result = append(result, pointing)
			queue = append(queue, pointing)
		}
	}

	return result
}

func (r *Records) GetAddresses(domainName string) []*Address {
	r.locker.RLock()
	defer r.locker.RUnlock()

	now := time.Now()
	seen := make(map[string]struct{})
	seen[domainName] = struct{}{}

	for {
		if addresses, ok := r.addresses[domainName]; ok && len(addresses) > 0 {
			// Фильтруем просроченные адреса (только чтение, без удаления)
			var valid []*Address
			for _, addr := range addresses {
				if !now.After(addr.Deadline) {
					valid = append(valid, addr)
				}
			}
			if len(valid) > 0 {
				return valid
			}
		}

		alias, ok := r.aliases[domainName]
		if !ok || now.After(alias.Deadline) {
			return nil
		}

		// Защита от циклов
		if _, ok := seen[alias.Alias]; ok {
			return nil
		}
		seen[alias.Alias] = struct{}{}
		domainName = alias.Alias
	}
}

func (r *Records) ListKnownDomains() []string {
	r.locker.RLock()
	defer r.locker.RUnlock()

	// Собираем уникальные домены из обоих map'ов
	domains := make(map[string]struct{}, len(r.addresses)+len(r.aliases))

	for name := range r.addresses {
		domains[name] = struct{}{}
	}

	for name := range r.aliases {
		domains[name] = struct{}{}
	}

	result := make([]string, 0, len(domains))
	for name := range domains {
		result = append(result, name)
	}
	return result
}

// cleanupRecords удаляет истёкшие записи
func (r *Records) cleanupRecords() {
	r.locker.Lock()
	defer r.locker.Unlock()

	now := time.Now()

	// Очистка адресов
	for name, addresses := range r.addresses {
		idx := 0
		for _, addr := range addresses {
			if !now.After(addr.Deadline) {
				addresses[idx] = addr
				idx++
			}
		}
		if idx == 0 {
			delete(r.addresses, name)
		} else {
			r.addresses[name] = addresses[:idx]
		}
	}

	// Очистка алиасов
	for name, alias := range r.aliases {
		if now.After(alias.Deadline) {
			r.removeReverseAlias(alias.Alias, name)
			delete(r.aliases, name)
		}
	}
}

// StartCleanup запускает фоновую очистку с заданным интервалом
func (r *Records) StartCleanup(ctx context.Context, interval time.Duration) {
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				r.cleanupRecords()
			case <-ctx.Done():
				return
			}
		}
	}()
}

func New() *Records {
	return &Records{
		addresses:      make(map[string][]*Address),
		aliases:        make(map[string]*Alias),
		reverseAliases: make(map[string][]string),
	}
}
