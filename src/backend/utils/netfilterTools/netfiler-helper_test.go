package netfilterTools

import "testing"

func TestNewRejectsShortPrefixes(t *testing.T) {
	tests := []struct {
		name         string
		chain, ipset string
		wantErr      bool
	}{
		{"defaults", "MT_", "mt_", false},
		{"longer", "MAGI_", "magi_", false},
		{"empty chain prefix", "", "mt_", true},
		{"empty ipset prefix", "MT_", "", true},
		{"two characters in chain prefix", "MT", "mt_", true},
		{"two characters in ipset prefix", "MT_", "mt", true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			h, err := New(tc.chain, tc.ipset, true, true, 1000)
			if (err != nil) != tc.wantErr {
				t.Fatalf("err = %v, wantErr %v", err, tc.wantErr)
			}
			if err != nil && h != nil {
				t.Error("helper returned together with an error")
			}
		})
	}
}
