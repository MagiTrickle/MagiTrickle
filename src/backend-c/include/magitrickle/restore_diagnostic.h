/* Bounded, read-only diagnostics for a failed iptables-restore transaction.
 * The input transcript is not written to disk or replayed. */
#ifndef MAGITRICKLE_RESTORE_DIAGNOSTIC_H
#define MAGITRICKLE_RESTORE_DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>

/* Logs the reported line and nearby context; returns without logging if
 * stderr lacks a valid 'line N' location. No allocations, all text bounded. */
void mt_ipt_log_restore_context(const char *cmd, const uint8_t *input, size_t input_len,
                                const uint8_t *stderr_data, size_t stderr_len);

#endif
