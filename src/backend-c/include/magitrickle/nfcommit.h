/* Netfilter table committer: a thread that owns writing the netfilter
 * tables, so that nobody has to wait for a write and no writer has to
 * finish what it started.
 *
 * Why this exists (Keenetic / Entware `_kn` builds): the firmware's own
 * userspace netfilter implementation rewrites a table whole and
 * atomically, then runs the `netfilter.d` hook, which POSTs to
 * /api/v1/system/hooks/netfilterd. Putting our rules back one chain at a
 * time from that hook races with the firmware's next rewrite: half of
 * what we wrote is gone before the other half lands, and whatever
 * iptables says about it comes back as an HTTP error nobody can act on.
 *
 * The contract here is the opposite of "commit and report":
 *
 *   - Requests accumulate. mt_nfcommit_request() never blocks and never
 *     reports anything; requests arriving while a rebuild runs collapse
 *     into a single following pass.
 *   - A request arriving mid-write aborts that write (the cancellation
 *     token kills the running iptables-restore) and starts a fresh one.
 *     Continuing to write into a table that was just replaced is worse
 *     than useless.
 *   - Nothing fails permanently. Any error just means the pass is
 *     restarted; errors from losing a race with the firmware are expected
 *     traffic, not incidents.
 *
 * The rebuild callback runs on the committer thread and must write the
 * table whole -- drop everything of ours first, then fill -- so that the
 * result never depends on what an aborted write left behind. It is
 * responsible for its own synchronization against the loop thread; see
 * mt_app_rebuild_netfilter (api/app.c) for the mutex this project uses,
 * which extends decisions.md D-19 rather than breaking it: the DNS hot
 * path still takes no locks, since it and the committer are both readers
 * of the ruleset registry and only the loop thread ever writes it.
 */
#ifndef MAGITRICKLE_NFCOMMIT_H
#define MAGITRICKLE_NFCOMMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/cancel.h"
#include "magitrickle/err.h"

typedef struct mt_nfcommit mt_nfcommit_t;

/* Runs one full rebuild. Should check `cancel` between steps and return
 * MT_ERR_CANCELED promptly once it is raised; any other non-MT_OK return
 * simply schedules another pass. */
typedef mt_err_t (*mt_nfcommit_rebuild_fn)(void *ud, mt_cancel_t *cancel);

/* NULL on OOM. The thread is not started yet -- requests made before
 * mt_nfcommit_start() are remembered and served by the first pass. */
mt_nfcommit_t *mt_nfcommit_new(mt_nfcommit_rebuild_fn fn, void *ud);

/* Stops the thread if running (aborting any write in flight) and frees.
 * NULL-safe. */
void mt_nfcommit_free(mt_nfcommit_t *c);

mt_err_t mt_nfcommit_start(mt_nfcommit_t *c);

/* Aborts the write in flight and joins the thread. Idempotent; NULL-safe.
 * Must happen before anything the rebuild callback touches is torn down. */
void mt_nfcommit_stop(mt_nfcommit_t *c);

/* Thread-safe, non-blocking: abort whatever is being written and rebuild
 * the table from scratch. NULL-safe, so callers on platforms without a
 * committer can call it unconditionally. */
void mt_nfcommit_request(mt_nfcommit_t *c);

/* Aborts the pass in flight without asking for extra work: an aborted
 * pass always reschedules itself, so this yields the netfilter state back
 * to the caller promptly (see api/app.c's app_nf_enter) without turning
 * every group edit into a rebuild of its own. NULL-safe; a no-op when no
 * pass is running.  */
void mt_nfcommit_interrupt(mt_nfcommit_t *c);

/* The token the committer aborts its rebuilds on. The rebuild callback
 * receives it as an argument; this accessor exists for callers that need
 * to reach it outside a pass (tests). Borrowed: valid until
 * mt_nfcommit_free. */
mt_cancel_t *mt_nfcommit_cancel(const mt_nfcommit_t *c);

/* Number of rebuild passes actually started. Test/observability hook. */
uint64_t mt_nfcommit_passes(const mt_nfcommit_t *c);

/* Overrides the settle delay before a pass and the ceiling that repeated
 * hard failures back off to. Tests only -- must be called before start. */
void mt_nfcommit_set_delays_for_test(mt_nfcommit_t *c, unsigned delay_ms, unsigned max_delay_ms);

#endif /* MAGITRICKLE_NFCOMMIT_H */
