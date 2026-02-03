package recordsCache

import (
	"bytes"
	"slices"
	"testing"
	"time"
)

func TestLoop(t *testing.T) {
	r := New()
	r.AddAlias("1", "2", 60)
	r.AddAlias("2", "1", 60)
	if r.GetAddresses("1") != nil {
		t.Fatal("loop detected")
	}
	if r.GetAddresses("2") != nil {
		t.Fatal("loop detected")
	}
}

func TestCName(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 60)
	r.AddAlias("gateway.example.com", "example.com", 60)
	records := r.GetAddresses("gateway.example.com")
	if records == nil {
		t.Fatal("no records")
	}
	if bytes.Compare(records[0].Address, []byte{1, 2, 3, 4}) != 0 {
		t.Fatal("cname mismatch")
	}
}

func TestA(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 60)
	records := r.GetAddresses("example.com")
	if records == nil {
		t.Fatal("no records")
	}
	if bytes.Compare(records[0].Address, []byte{1, 2, 3, 4}) != 0 {
		t.Fatal("cname mismatch")
	}
}

func TestDeprecated(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 0)
	time.Sleep(time.Second)
	records := r.GetAddresses("example.com")
	if records != nil {
		t.Fatal("deprecated records")
	}
}

func TestNotExistedA(t *testing.T) {
	r := New()
	records := r.GetAddresses("example.com")
	if records != nil {
		t.Fatal("not existed records")
	}
}

func TestNotExistedCNameAlias(t *testing.T) {
	r := New()
	r.AddAlias("gateway.example.com", "example.com", 60)
	records := r.GetAddresses("gateway.example.com")
	if records != nil {
		t.Fatal("not existed records")
	}
}

func TestReplacing(t *testing.T) {
	r := New()
	r.AddAlias("gateway.example.com", "example.com", 60)
	r.AddAddress("gateway.example.com", []byte{1, 2, 3, 4}, 60)
	records := r.GetAddresses("gateway.example.com")
	if bytes.Compare(records[0].Address, []byte{1, 2, 3, 4}) != 0 {
		t.Fatal("mismatch")
	}
}

func TestAliases(t *testing.T) {
	r := New()
	r.AddAddress("1", []byte{1, 2, 3, 4}, 60)
	r.AddAlias("2", "1", 60)
	r.AddAlias("3", "2", 60)
	r.AddAlias("4", "2", 60)
	r.AddAlias("5", "1", 60)
	aliases := r.GetAliases("1")
	if aliases == nil {
		t.Fatal("no aliases")
	}
	if !slices.Contains(aliases, "1") {
		t.Fatal("no 1")
	}
	if !slices.Contains(aliases, "2") {
		t.Fatal("no 2")
	}
	if !slices.Contains(aliases, "3") {
		t.Fatal("no 3")
	}
	if !slices.Contains(aliases, "4") {
		t.Fatal("no 4")
	}
	if !slices.Contains(aliases, "5") {
		t.Fatal("no 5")
	}
}

func TestMultipleAddresses(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 60)
	r.AddAddress("example.com", []byte{5, 6, 7, 8}, 60)
	r.AddAddress("example.com", []byte{9, 10, 11, 12}, 60)

	records := r.GetAddresses("example.com")
	if len(records) != 3 {
		t.Fatalf("expected 3 addresses, got %d", len(records))
	}
}

func TestAddressDeadlineUpdate(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 1)
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 60) // same IP, longer TTL

	records := r.GetAddresses("example.com")
	if len(records) != 1 {
		t.Fatalf("expected 1 address, got %d", len(records))
	}

	time.Sleep(time.Second * 2)
	records = r.GetAddresses("example.com")
	if records == nil {
		t.Fatal("address should still be valid after TTL update")
	}
}

func TestAliasUpdate(t *testing.T) {
	r := New()
	r.AddAddress("target1", []byte{1, 1, 1, 1}, 60)
	r.AddAddress("target2", []byte{2, 2, 2, 2}, 60)

	r.AddAlias("alias", "target1", 60)
	records := r.GetAddresses("alias")
	if !bytes.Equal(records[0].Address, []byte{1, 1, 1, 1}) {
		t.Fatal("should resolve to target1")
	}

	// Update alias to point to target2
	r.AddAlias("alias", "target2", 60)
	records = r.GetAddresses("alias")
	if !bytes.Equal(records[0].Address, []byte{2, 2, 2, 2}) {
		t.Fatal("should resolve to target2 after update")
	}

	// Verify reverse index updated correctly
	aliases1 := r.GetAliases("target1")
	if slices.Contains(aliases1, "alias") {
		t.Fatal("alias should not point to target1 anymore")
	}

	aliases2 := r.GetAliases("target2")
	if !slices.Contains(aliases2, "alias") {
		t.Fatal("alias should point to target2")
	}
}

func TestExpiredAliasInChain(t *testing.T) {
	r := New()
	r.AddAddress("target", []byte{1, 2, 3, 4}, 60)
	r.AddAlias("middle", "target", 0) // expires immediately
	r.AddAlias("start", "middle", 60)

	time.Sleep(time.Second)

	records := r.GetAddresses("start")
	if records != nil {
		t.Fatal("should not resolve through expired alias")
	}
}

func TestDeepCNameChain(t *testing.T) {
	r := New()
	r.AddAddress("level5", []byte{5, 5, 5, 5}, 60)
	r.AddAlias("level4", "level5", 60)
	r.AddAlias("level3", "level4", 60)
	r.AddAlias("level2", "level3", 60)
	r.AddAlias("level1", "level2", 60)

	records := r.GetAddresses("level1")
	if records == nil {
		t.Fatal("should resolve deep chain")
	}
	if !bytes.Equal(records[0].Address, []byte{5, 5, 5, 5}) {
		t.Fatal("wrong address from deep chain")
	}
}

func TestListKnownDomains(t *testing.T) {
	r := New()
	r.AddAddress("domain1.com", []byte{1, 1, 1, 1}, 60)
	r.AddAddress("domain2.com", []byte{2, 2, 2, 2}, 60)
	r.AddAlias("alias1.com", "domain1.com", 60)
	r.AddAlias("alias2.com", "domain2.com", 60)

	domains := r.ListKnownDomains()
	if len(domains) != 4 {
		t.Fatalf("expected 4 domains, got %d", len(domains))
	}

	expected := []string{"domain1.com", "domain2.com", "alias1.com", "alias2.com"}
	for _, e := range expected {
		if !slices.Contains(domains, e) {
			t.Fatalf("missing domain: %s", e)
		}
	}
}

func TestSelfAlias(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 2, 3, 4}, 60)
	r.AddAlias("example.com", "example.com", 60) // self-reference should be ignored

	records := r.GetAddresses("example.com")
	if records == nil {
		t.Fatal("self-alias should not break resolution")
	}
	if !bytes.Equal(records[0].Address, []byte{1, 2, 3, 4}) {
		t.Fatal("wrong address")
	}
}

func TestPartialExpiredAddresses(t *testing.T) {
	r := New()
	r.AddAddress("example.com", []byte{1, 1, 1, 1}, 0)  // expires immediately
	r.AddAddress("example.com", []byte{2, 2, 2, 2}, 60) // valid

	time.Sleep(time.Second)

	records := r.GetAddresses("example.com")
	if len(records) != 1 {
		t.Fatalf("expected 1 valid address, got %d", len(records))
	}
	if !bytes.Equal(records[0].Address, []byte{2, 2, 2, 2}) {
		t.Fatal("wrong address returned")
	}
}

func TestCleanup(t *testing.T) {
	r := New()
	r.AddAddress("expired.com", []byte{1, 1, 1, 1}, 0)
	r.AddAddress("valid.com", []byte{2, 2, 2, 2}, 60)
	r.AddAlias("expired-alias.com", "valid.com", 0)
	r.AddAlias("valid-alias.com", "valid.com", 60)

	time.Sleep(time.Second)
	r.cleanupRecords()

	domains := r.ListKnownDomains()
	if slices.Contains(domains, "expired.com") {
		t.Fatal("expired address should be cleaned up")
	}
	if slices.Contains(domains, "expired-alias.com") {
		t.Fatal("expired alias should be cleaned up")
	}
	if !slices.Contains(domains, "valid.com") {
		t.Fatal("valid address should remain")
	}
	if !slices.Contains(domains, "valid-alias.com") {
		t.Fatal("valid alias should remain")
	}
}

func TestGetAliasesNoReverse(t *testing.T) {
	r := New()
	r.AddAddress("standalone.com", []byte{1, 2, 3, 4}, 60)

	aliases := r.GetAliases("standalone.com")
	if len(aliases) != 1 {
		t.Fatalf("expected 1 alias (itself), got %d", len(aliases))
	}
	if aliases[0] != "standalone.com" {
		t.Fatal("should contain itself")
	}
}

func TestGetAliasesUnknownDomain(t *testing.T) {
	r := New()
	aliases := r.GetAliases("unknown.com")
	if len(aliases) != 1 || aliases[0] != "unknown.com" {
		t.Fatal("unknown domain should return only itself")
	}
}
