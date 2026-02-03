package netfilterTools

import (
	"errors"
	"fmt"
	"strings"

	"magitrickle/utils/iptables"
)

func (nh *Helper) cleanIPTables(ipt *iptables.IPTables) error {
	if ipt == nil {
		return nil
	}
	jumpToChainPrefix := "-j " + nh.ChainPrefix

	exists, err := ipt.GetCurrentRules()
	if err != nil {
		return fmt.Errorf("listing chains error: %w", err)
	}

	for table, chains := range exists {
		chainListToDelete := make([]string, 0)

		for chain, rules := range chains {
			if strings.HasPrefix(chain, nh.ChainPrefix) {
				chainListToDelete = append(chainListToDelete, chain)
				continue
			}

			for _, r := range rules {
				if !r.Contains(jumpToChainPrefix) {
					continue
				}

				err = ipt.Delete(table, chain, r.Args()...)
				if errors.Is(err, iptables.ErrChainNotInitialized) {
					err = ipt.RegisterChainPatch(table, chain)
					if err != nil {
						return fmt.Errorf("chain register error: %w", err)
					}
					err = ipt.Delete(table, chain, r.Args()...)
				}
				if err != nil {
					return fmt.Errorf("rule deletion error: %w", err)
				}
			}
		}

		for _, chain := range chainListToDelete {
			err = ipt.RegisterChainDelete(table, chain)
			if err != nil {
				return fmt.Errorf("deleting chain error: %w", err)
			}
		}
	}

	err = ipt.Commit()
	if err != nil {
		return fmt.Errorf("failed to commit iptables rules: %w", err)
	}
	return nil
}

func (nh *Helper) CleanIPTables() error {
	var errs []error
	errs = append(errs, nh.cleanIPTables(nh.IPTables4))
	errs = append(errs, nh.cleanIPTables(nh.IPTables6))
	return errors.Join(errs...)
}
