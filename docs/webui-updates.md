# GitHub updates from WebUI

## Flow

WebUI loads `dan0102dan/mt-c/releases?per_page=100` once into a shared one-hour
browser cache (ETag/backoff). Stable chooses the most recently published non-draft,
non-prerelease entry; dev also includes prereleases. Channel switches stay local.
Opening the version popover refreshes expired metadata; errors expose Retry.
Router tokens are never sent to GitHub. Actions-only artifacts are not updates.

On Update, **the existing daemon** prepares the package on a background thread.
It independently re-fetches that same release list and verifies release ID/tag,
newer version/revision, exact build/firmware target, official download URL, size
and SHA-256 over HTTPS. It saves the current configuration before preparation.
No download or package-manager wait runs in the DNS/HTTP event loop.

Once verified, a detached **system `/bin/sh` command** invokes the configured
`opkg install` or `apk add --allow-untrusted`, restarts the existing init service
on success, and removes its private temporary package. There is no `mt-c-updater`
binary, alternate daemon mode, or second running `magitrickled`. Existing Entware
`rc.func` and OpenWrt `procd` service scripts are unchanged. The shell command is
fixed; URLs, commands and package paths cannot be supplied by the browser.

The shell starts a package manager only after a startup handshake completes.
Its stdin/stdout/stderr and inherited descriptors do not depend on the daemon.
The update lock stays in the system shell, not in package scripts or the restarted
daemon. Stopping the original daemon/process group therefore does not cancel a
live package transaction. Preparation is canceled/joined on daemon shutdown;
a transaction already handed off is never killed or automatically repeated.

## State and diagnostics

Preparation status and errors exist **in memory only**. No request/status JSON,
`install.log`, dedicated backup or result history is created. Existing package
conffile handling preserves settings. Package-manager output goes to the system
console; the application does not promise persistent system logging. Detailed
installation diagnostics and manual recovery are the administrator's CLI task.
There is no automatic binary rollback or power-loss recovery transaction.

`<state-dir>/update/lock` is retained solely for `flock` serialization, including
an upgrade launched by the former separate updater. Old result/backup files are
not read, overwritten or deleted. `/tmp/mt-c-update-*` contains only the verified
package until the system command finishes. Power loss may interrupt installation;
the next daemon does not resume an old operation. Old binaries omitted from a new
package are removed by the package manager, not an ad-hoc cleanup command.

WebUI retains only its expected version/revision in **per-tab session storage**,
not errors or an installation log. After restart, it waits for the installer lock
to be released and reads the running daemon's version. Matching the expected
version/revision confirms the running version, not the complete package-manager
transaction. An unchanged version is not success. Transport failures retry only
GETs, never POST. A closed tab without saved intent simply sees the current version.
On the first upgrade from the legacy updater, an already-loaded old WebUI may need
a manual page reload to display the new version.

## API (D-74, amended by D-75)

- `GET /api/v1/system/update`: local `installed_version`, `installed_revision`,
  `asset_suffix`, `can_install`, `reason`; no GitHub request.
- `GET /api/v1/system/update/status`: in-memory `queued`, `checking`, `downloading`,
  `verifying`, `installing` or `failed`, with `job_id`, `tag`, target version/revision
  and optional transient `error`. Without a job it returns `{"stage":"idle"}`.
  A freshly restarted daemon while the installer holds the lock returns
  `{"stage":"installing","external":true}` **without** a job ID. A free lock is
  `idle`, not proof of successful installation. Lock access/allocation errors: 500.
- `POST /api/v1/system/update/install`: exactly
  `{"tag":"0.9.0","release_id":123,"preview":false}`. 202 with `job_id` and
  `stage:"queued"` acknowledges an accepted preparation thread, not successful
  download/exec/install. Bad/non-newer input: 400; root/target/manager unavailable:
  403; another job: 409; config save/lock/allocation/thread creation failure: 500.

Both GET routes return `Cache-Control: no-store`. Installation requires a verified
root JWT on TCP and Unix sockets, including when ordinary WebUI auth is disabled.
Existing auth middleware may first return 401. Errors retain `{"error":"..."}`.
Numeric two-to-four-component tags and package revisions are compared numerically;
installed `~git...` and APK `_pre...` versions sort before their matching release.
Unknown formats, unavailable exact targets, missing digests and downgrades fail closed.

## Tests and device checks

C release tests cover versions/channels/assets; subprocess tests use **synthetic**
manager/init scripts to verify parent process-group death, inherited-fd isolation,
exclusive locking, cleanup and no restart on manager failure. They never install a
real package. Packaging tests assert absence of the old updater from IPK/APK payloads.
Deno mock routes simulate preparation followed by an idle new daemon/version.
Frontend unit/Playwright tests cover shared cache, Retry, volatile status after
restart, per-tab intent, version-read failure and an unchanged version after handoff.

Before production rollout, test real Entware/Keenetic and OpenWrt IPK/APK upgrades,
including the first migration from the old updater, conffile preservation, package
hook restarts and a failing package manager. Host tests/CI do not prove device
service-manager behavior or guarantee recovery from power loss.
