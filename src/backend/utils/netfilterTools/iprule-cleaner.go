package netfilterTools

import (
	"errors"
	"fmt"
	"os"
	"strings"

	"github.com/vishvananda/netlink"
	"github.com/vishvananda/netlink/nl"
	"golang.org/x/sys/unix"
)

// CleanIPRules removes the policy rules and routing tables a previous instance left behind when it was killed
// without a chance to clean up. Our rules map fwmark N to table N, both starting at StartIdx, and every table of
// ours holds a blackhole default route (see IPSetToLink.insertIPRoute). Only rules whose table has
// that route are touched: the indexes are not contiguous, and a rule that merely looks similar may belong to
// other software.
func (nh *Helper) CleanIPRules() error {
	rules, err := netlink.RuleList(nl.FAMILY_ALL)
	if err != nil {
		return fmt.Errorf("error while getting rules: %w", err)
	}

	var errs []error
	routesByTable := make(map[int][]netlink.Route)
	ours := make(map[int]bool)
	for i := range rules {
		rule := &rules[i]
		if rule.Mark == 0 || rule.Mark < nh.StartIdx || rule.Table != int(rule.Mark) {
			continue
		}

		isOurs, seen := ours[rule.Table]
		if !seen {
			routes, err := netlink.RouteListFiltered(nl.FAMILY_ALL, &netlink.Route{Table: rule.Table}, netlink.RT_FILTER_TABLE)
			if err != nil {
				errs = append(errs, fmt.Errorf("error while listing routes of table %d: %w", rule.Table, err))
				ours[rule.Table] = false
				continue
			}
			isOurs = hasOwnBlackholeRoute(routes)
			ours[rule.Table], routesByTable[rule.Table] = isOurs, routes
		}
		if !isOurs {
			continue
		}

		if err := netlink.RuleDel(rule); err != nil && !errors.Is(err, unix.ENOENT) {
			errs = append(errs, fmt.Errorf("error while deleting stale rule: %w", err))
		}
	}

	// The routes go only after all rules of the table (IPv4 and IPv6) are checked and deleted.
	for table, routes := range routesByTable {
		if !ours[table] {
			continue
		}
		for j := range routes {
			if err := netlink.RouteDel(&routes[j]); err != nil && !errors.Is(err, unix.ESRCH) {
				errs = append(errs, fmt.Errorf("error while deleting stale route: %w", err))
			}
		}
	}
	return errors.Join(errs...)
}

// hasOwnBlackholeRoute reports whether the table holds the blackhole default route that marks it as ours.
func hasOwnBlackholeRoute(routes []netlink.Route) bool {
	for i := range routes {
		r := &routes[i]
		if r.Type != unix.RTN_BLACKHOLE || r.Priority != blackholeRouteMetric {
			continue
		}
		if r.Dst == nil {
			return true
		}
		if ones, _ := r.Dst.Mask.Size(); ones == 0 {
			return true
		}
	}
	return false
}

// CleanIPSets destroys the sets of a previous instance that did not get to remove them. It must run after
// CleanIPTables, as a set that a rule still references cannot be destroyed.
func (nh *Helper) CleanIPSets() error {
	sets, err := netlink.IpsetListAll()
	if err != nil {
		return fmt.Errorf("error while listing ipsets: %w", err)
	}

	var errs []error
	for _, set := range sets {
		if !strings.HasPrefix(set.SetName, nh.IpsetPrefix) {
			continue
		}
		if err := netlink.IpsetDestroy(set.SetName); err != nil && !os.IsNotExist(err) {
			errs = append(errs, fmt.Errorf("error while destroying stale ipset %s: %w", set.SetName, err))
		}
	}
	return errors.Join(errs...)
}
