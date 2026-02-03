//go:build testing

package iptables

import (
	"reflect"
	"testing"
)

// TestChainPatch проверяет, что правила добавляются к существующим
func TestChainPatch(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Начальное состояние: в chain уже есть правила
	fake.SetInitialRules("filter", "FORWARD", [][]string{
		{"-i", "eth0", "-j", "ACCEPT"},
		{"-i", "eth1", "-j", "DROP"},
	})

	ipt := NewIPTables(fake)

	// Регистрируем chain как overlay
	err := ipt.RegisterChainPatch("filter", "FORWARD")
	if err != nil {
		t.Fatalf("RegisterChainPatch failed: %v", err)
	}

	// Добавляем новое правило
	err = ipt.Append("filter", "FORWARD", "-i", "eth2", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Append failed: %v", err)
	}

	// Коммитим
	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	// Проверяем результат: старые правила должны остаться, новое добавиться в конец
	rules := fake.GetRules("filter", "FORWARD")
	expected := [][]string{
		{"-i", "eth0", "-j", "ACCEPT"},
		{"-i", "eth1", "-j", "DROP"},
		{"-i", "eth2", "-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Patch failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainPatchDelete проверяет удаление правил в overlay режиме
func TestChainPatchDelete(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("filter", "INPUT", [][]string{
		{"-p", "tcp", "--dport", "22", "-j", "ACCEPT"},
		{"-p", "tcp", "--dport", "80", "-j", "ACCEPT"},
		{"-p", "tcp", "--dport", "443", "-j", "ACCEPT"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainPatch("filter", "INPUT")
	if err != nil {
		t.Fatalf("RegisterChainPatch failed: %v", err)
	}

	// Удаляем правило для порта 80
	err = ipt.Delete("filter", "INPUT", "-p", "tcp", "--dport", "80", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Delete failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "INPUT")
	expected := [][]string{
		{"-p", "tcp", "--dport", "22", "-j", "ACCEPT"},
		{"-p", "tcp", "--dport", "443", "-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Patch delete failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainPatchInsert проверяет вставку правил в начало
func TestChainPatchInsert(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("filter", "OUTPUT", [][]string{
		{"-d", "10.0.0.0/8", "-j", "ACCEPT"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainPatch("filter", "OUTPUT")
	if err != nil {
		t.Fatalf("RegisterChainPatch failed: %v", err)
	}

	// Вставляем правило в начало (позиция 1)
	err = ipt.Insert("filter", "OUTPUT", 1, "-d", "192.168.0.0/16", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Insert failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "OUTPUT")
	expected := [][]string{
		{"-d", "192.168.0.0/16", "-j", "ACCEPT"},
		{"-d", "10.0.0.0/8", "-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Patch insert failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainPatchNoDuplicates проверяет, что дубликаты не добавляются
func TestChainPatchNoDuplicates(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("filter", "FORWARD", [][]string{
		{"-i", "eth0", "-j", "ACCEPT"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainPatch("filter", "FORWARD")
	if err != nil {
		t.Fatalf("RegisterChainPatch failed: %v", err)
	}

	// Пытаемся добавить уже существующее правило
	err = ipt.Append("filter", "FORWARD", "-i", "eth0", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Append failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "FORWARD")
	expected := [][]string{
		{"-i", "eth0", "-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Patch should not duplicate rules.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainOverride проверяет полную замену chain
func TestChainOverride(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Начальное состояние: chain с правилами
	fake.SetInitialRules("nat", "PREROUTING", [][]string{
		{"-p", "tcp", "--dport", "80", "-j", "REDIRECT", "--to-port", "8080"},
		{"-p", "tcp", "--dport", "443", "-j", "REDIRECT", "--to-port", "8443"},
	})

	ipt := NewIPTables(fake)

	// Регистрируем как override - полная замена
	err := ipt.RegisterChainOverride("nat", "PREROUTING")
	if err != nil {
		t.Fatalf("RegisterChainOverride failed: %v", err)
	}

	// Добавляем новые правила (они заменят старые)
	err = ipt.Append("nat", "PREROUTING", "-p", "udp", "--dport", "53", "-j", "REDIRECT", "--to-port", "5353")
	if err != nil {
		t.Fatalf("Append failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	// Проверяем: старые правила должны исчезнуть, остаться только новое
	rules := fake.GetRules("nat", "PREROUTING")
	expected := [][]string{
		{"-p", "udp", "--dport", "53", "-j", "REDIRECT", "--to-port", "5353"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Override failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainOverrideMultipleRules проверяет замену несколькими правилами
func TestChainOverrideMultipleRules(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("mangle", "PREROUTING", [][]string{
		{"-j", "OLD_CHAIN"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainOverride("mangle", "PREROUTING")
	if err != nil {
		t.Fatalf("RegisterChainOverride failed: %v", err)
	}

	// Добавляем несколько правил
	err = ipt.Append("mangle", "PREROUTING", "-j", "MARK", "--set-mark", "1")
	if err != nil {
		t.Fatalf("Append 1 failed: %v", err)
	}

	err = ipt.Append("mangle", "PREROUTING", "-j", "MARK", "--set-mark", "2")
	if err != nil {
		t.Fatalf("Append 2 failed: %v", err)
	}

	err = ipt.Append("mangle", "PREROUTING", "-j", "CONNMARK", "--save-mark")
	if err != nil {
		t.Fatalf("Append 3 failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("mangle", "PREROUTING")
	expected := [][]string{
		{"-j", "MARK", "--set-mark", "1"},
		{"-j", "MARK", "--set-mark", "2"},
		{"-j", "CONNMARK", "--save-mark"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Override with multiple rules failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainOverrideNoChangeIfSame проверяет, что если правила не изменились, ничего не происходит
func TestChainOverrideNoChangeIfSame(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("filter", "TEST", [][]string{
		{"-j", "ACCEPT"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainOverride("filter", "TEST")
	if err != nil {
		t.Fatalf("RegisterChainOverride failed: %v", err)
	}

	// Добавляем то же самое правило
	err = ipt.Append("filter", "TEST", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Append failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "TEST")
	expected := [][]string{
		{"-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Override same rules failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestChainDelete проверяет удаление chain
func TestChainDelete(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Создаём chain с правилами
	fake.SetInitialRules("filter", "MY_CHAIN", [][]string{
		{"-j", "ACCEPT"},
		{"-j", "DROP"},
	})

	ipt := NewIPTables(fake)

	// Регистрируем для удаления
	err := ipt.RegisterChainDelete("filter", "MY_CHAIN")
	if err != nil {
		t.Fatalf("RegisterChainDelete failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	// Проверяем, что chain удалён
	if fake.ChainExists("filter", "MY_CHAIN") {
		t.Error("Chain should be deleted but still exists")
	}
}

// TestChainDeleteEmpty проверяет удаление пустого chain
func TestChainDeleteEmpty(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Создаём пустой chain
	fake.SetInitialRules("filter", "EMPTY_CHAIN", [][]string{})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainDelete("filter", "EMPTY_CHAIN")
	if err != nil {
		t.Fatalf("RegisterChainDelete failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	if fake.ChainExists("filter", "EMPTY_CHAIN") {
		t.Error("Empty chain should be deleted but still exists")
	}
}

// TestChainDeleteNonExistent проверяет, что удаление несуществующего chain не вызывает ошибок
func TestChainDeleteNonExistent(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// НЕ создаём chain - он не существует

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainDelete("filter", "NON_EXISTENT")
	if err != nil {
		t.Fatalf("RegisterChainDelete failed: %v", err)
	}

	// Commit не должен падать
	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	// Chain по-прежнему не существует
	if fake.ChainExists("filter", "NON_EXISTENT") {
		t.Error("Non-existent chain should not be created")
	}
}

// TestChainDeleteIgnoresAppend проверяет, что Append/Insert/Delete игнорируются для удаляемого chain
func TestChainDeleteIgnoresAppend(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	fake.SetInitialRules("filter", "TO_DELETE", [][]string{
		{"-j", "ACCEPT"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainDelete("filter", "TO_DELETE")
	if err != nil {
		t.Fatalf("RegisterChainDelete failed: %v", err)
	}

	// Эти операции должны быть проигнорированы
	_ = ipt.Append("filter", "TO_DELETE", "-j", "DROP")
	_ = ipt.Insert("filter", "TO_DELETE", 1, "-j", "LOG")
	_ = ipt.Delete("filter", "TO_DELETE", "-j", "ACCEPT")

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	if fake.ChainExists("filter", "TO_DELETE") {
		t.Error("Chain should be deleted")
	}
}

// TestMixedChainTypes проверяет работу разных типов chain в одной таблице
func TestMixedChainTypes(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Начальное состояние
	fake.SetInitialRules("filter", "INPUT", [][]string{
		{"-j", "ACCEPT"},
	})
	fake.SetInitialRules("filter", "FORWARD", [][]string{
		{"-j", "OLD_RULE"},
	})
	fake.SetInitialRules("filter", "TO_DELETE", [][]string{
		{"-j", "SOMETHING"},
	})

	ipt := NewIPTables(fake)

	// INPUT - overlay (добавляем к существующим)
	_ = ipt.RegisterChainPatch("filter", "INPUT")
	_ = ipt.Append("filter", "INPUT", "-j", "DROP")

	// FORWARD - override (заменяем полностью)
	_ = ipt.RegisterChainOverride("filter", "FORWARD")
	_ = ipt.Append("filter", "FORWARD", "-j", "NEW_RULE")

	// TO_DELETE - удаляем
	_ = ipt.RegisterChainDelete("filter", "TO_DELETE")

	err := ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	// Проверяем INPUT (overlay)
	inputRules := fake.GetRules("filter", "INPUT")
	expectedInput := [][]string{
		{"-j", "ACCEPT"},
		{"-j", "DROP"},
	}
	if !reflect.DeepEqual(inputRules, expectedInput) {
		t.Errorf("INPUT overlay failed.\nExpected: %v\nGot: %v", expectedInput, inputRules)
	}

	// Проверяем FORWARD (override)
	forwardRules := fake.GetRules("filter", "FORWARD")
	expectedForward := [][]string{
		{"-j", "NEW_RULE"},
	}
	if !reflect.DeepEqual(forwardRules, expectedForward) {
		t.Errorf("FORWARD override failed.\nExpected: %v\nGot: %v", expectedForward, forwardRules)
	}

	// Проверяем TO_DELETE (должен быть удалён)
	if fake.ChainExists("filter", "TO_DELETE") {
		t.Error("TO_DELETE chain should not exist")
	}
}

// TestMultipleCommits проверяет несколько последовательных коммитов
func TestMultipleCommits(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	ipt := NewIPTables(fake)

	// Первый коммит: создаём chain
	_ = ipt.RegisterChainOverride("filter", "MY_CHAIN")
	_ = ipt.Append("filter", "MY_CHAIN", "-j", "ACCEPT")

	err := ipt.Commit()
	if err != nil {
		t.Fatalf("First commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "MY_CHAIN")
	if len(rules) != 1 || rules[0][1] != "ACCEPT" {
		t.Errorf("After first commit: %v", rules)
	}

	// Второй коммит: добавляем ещё правило
	_ = ipt.Append("filter", "MY_CHAIN", "-j", "DROP")

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Second commit failed: %v", err)
	}

	rules = fake.GetRules("filter", "MY_CHAIN")
	expected := [][]string{
		{"-j", "ACCEPT"},
		{"-j", "DROP"},
	}
	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("After second commit.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestIPv6 проверяет работу с IPv6
func TestIPv6(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv6)

	ipt := NewIPTables(fake)

	if ipt.Proto() != ProtocolIPv6 {
		t.Error("Protocol should be IPv6")
	}

	_ = ipt.RegisterChainOverride("filter", "INPUT")
	_ = ipt.Append("filter", "INPUT", "-s", "::1", "-j", "ACCEPT")

	err := ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "INPUT")
	expected := [][]string{
		{"-s", "::1", "-j", "ACCEPT"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("IPv6 rules failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}

// TestErrorOnUninitializedChain проверяет ошибку при работе с незарегистрированным chain
func TestErrorOnUninitializedChain(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)
	ipt := NewIPTables(fake)

	err := ipt.Append("filter", "NONEXISTENT", "-j", "ACCEPT")
	if err != ErrChainNotInitialized {
		t.Errorf("Expected ErrChainNotInitialized, got: %v", err)
	}

	err = ipt.Insert("filter", "NONEXISTENT", 1, "-j", "ACCEPT")
	if err != ErrChainNotInitialized {
		t.Errorf("Expected ErrChainNotInitialized, got: %v", err)
	}

	err = ipt.Delete("filter", "NONEXISTENT", "-j", "ACCEPT")
	if err != ErrChainNotInitialized {
		t.Errorf("Expected ErrChainNotInitialized, got: %v", err)
	}
}

// TestPatchRemovesDuplicates проверяет удаление дубликатов в overlay
func TestPatchRemovesDuplicates(t *testing.T) {
	fake := NewFakeIPTables(ProtocolIPv4)

	// Начальное состояние с дубликатами
	fake.SetInitialRules("filter", "FORWARD", [][]string{
		{"-j", "ACCEPT"},
		{"-j", "ACCEPT"}, // дубликат
		{"-j", "DROP"},
	})

	ipt := NewIPTables(fake)

	err := ipt.RegisterChainPatch("filter", "FORWARD")
	if err != nil {
		t.Fatalf("RegisterChainPatch failed: %v", err)
	}

	// Append того же правила должен удалить дубликаты, но не добавить новое
	err = ipt.Append("filter", "FORWARD", "-j", "ACCEPT")
	if err != nil {
		t.Fatalf("Append failed: %v", err)
	}

	err = ipt.Commit()
	if err != nil {
		t.Fatalf("Commit failed: %v", err)
	}

	rules := fake.GetRules("filter", "FORWARD")
	// Должен остаться только один -j ACCEPT и один -j DROP
	expected := [][]string{
		{"-j", "ACCEPT"},
		{"-j", "DROP"},
	}

	if !reflect.DeepEqual(rules, expected) {
		t.Errorf("Patch duplicate removal failed.\nExpected: %v\nGot: %v", expected, rules)
	}
}
