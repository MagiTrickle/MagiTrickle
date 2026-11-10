package netfilterTools

import "testing"

func TestHostCIDRMatchesWrittenHost(t *testing.T) {
	cases := []struct{ cidr, bits, want uint8 }{
		{32, 32, 0},   // хост, прочитанный из ядра
		{0, 32, 0},    // хост, если ядро маску не вернуло
		{24, 32, 24},  // подсеть не трогаем
		{128, 128, 0}, // хост IPv6
		{64, 128, 64},
	}
	for _, c := range cases {
		if got := hostCIDR(c.cidr, c.bits); got != c.want {
			t.Errorf("hostCIDR(%d, %d) = %d, want %d", c.cidr, c.bits, got, c.want)
		}
	}
}

func TestIsOwnIPSetMatchesOnlyAppNames(t *testing.T) {
	for name, want := range map[string]bool{
		"mt_0a1b2c3d-0_4":     true,
		"mt_0a1b2c3d-1_6":     true,
		"mt_0a1b2c3d_4":       true, // старый формат без слота
		"mt_sub-0a1b2c3d_6":   true, // старый формат подписок
		"mt_vpn_hosts":        false,
		"mt_0a1b2c3d-0":       false,
		"mt_0A1B2C3D-0_4":     false,
		"other_0a1b2c3d-0_4":  false,
		"mt_0a1b2c3d-0_4_bak": false,
	} {
		if got := isOwnIPSet(name, "mt_"); got != want {
			t.Errorf("isOwnIPSet(%q) = %v, want %v", name, got, want)
		}
	}
}
