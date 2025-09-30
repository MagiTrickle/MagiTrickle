package recordsCache

import (
	"bytes"
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
	locker  sync.Mutex
	records map[string]interface{}
}

func (r *Records) AddAlias(domainName, alias string, ttl uint32) {
	if domainName == alias {
		return
	}

	r.locker.Lock()
	r.records[domainName] = &Alias{
		Alias:    alias,
		Deadline: time.Now().Add(time.Duration(ttl) * time.Second),
	}
	r.locker.Unlock()
}

func (r *Records) AddAddress(domainName string, addr net.IP, ttl uint32) {
	r.locker.Lock()
	defer r.locker.Unlock()

	deadline := time.Now().Add(time.Duration(ttl) * time.Second)

	address, _ := r.records[domainName].([]*Address)
	for _, aRecord := range address {
		if bytes.Compare(aRecord.Address, addr) != 0 {
			continue
		}
		aRecord.Deadline = deadline
		return
	}

	r.records[domainName] = append(address, &Address{
		Address:  addr,
		Deadline: deadline,
	})
}

func (r *Records) GetAliases(domainName string) []string {
	r.locker.Lock()
	defer r.locker.Unlock()
	r.cleanupRecords()

	domains := make(map[string]struct{})
	domains[domainName] = struct{}{}

	for {
		var addedNew bool
		for name, record := range r.records {
			if _, ok := domains[name]; ok {
				continue
			}
			cname, ok := record.(*Alias)
			if !ok {
				continue
			}
			if _, ok = domains[cname.Alias]; !ok {
				continue
			}

			domains[name] = struct{}{}
			addedNew = true
		}
		if !addedNew {
			break
		}
	}

	domainList := make([]string, len(domains))
	idx := 0
	for name := range domains {
		domainList[idx] = name
		idx++
	}

	return domainList
}

func (r *Records) GetAddresses(domainName string) []*Address {
	r.locker.Lock()
	defer r.locker.Unlock()
	r.cleanupRecords()

	loopDetect := make(map[string]struct{})
	loopDetect[domainName] = struct{}{}
	for {
		switch v := r.records[domainName].(type) {
		case *Alias:
			if _, ok := loopDetect[v.Alias]; ok {
				return nil
			}
			domainName = v.Alias
			loopDetect[v.Alias] = struct{}{}
		case []*Address:
			return v
		default:
			return nil
		}
	}
}

func (r *Records) ListKnownDomains() []string {
	r.locker.Lock()
	defer r.locker.Unlock()
	r.cleanupRecords()

	domainsList := make([]string, len(r.records))
	i := 0
	for name := range r.records {
		domainsList[i] = name
		i++
	}
	return domainsList
}

func (r *Records) cleanupRecords() {
	now := time.Now()
	for name, records := range r.records {
		switch v := records.(type) {
		case []*Address:
			idx := 0
			for _, aRecord := range v {
				if now.After(aRecord.Deadline) {
					continue
				}
				v[idx] = aRecord
				idx++
			}
			if idx == 0 {
				delete(r.records, name)
				break
			}
			r.records[name] = v[:idx]
		case *Alias:
			if !now.After(v.Deadline) {
				continue
			}
			delete(r.records, name)
		}
	}
}

func New() *Records {
	return &Records{
		records: make(map[string]interface{}),
	}
}
