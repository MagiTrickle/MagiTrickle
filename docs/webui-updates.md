# GitHub updates from WebUI

## Responsibility and user flow

The browser checks `dan0102dan/mt-c` GitHub Releases directly. No release-check
scheduler or GitHub cache is added to the routing daemon. A shared one-hour
browser cache supports conditional ETag requests, overlapping request coalescing
and rate-limit backoff. Router credentials are never sent to GitHub.
Clicking the header version opens a compact popup with the update button,
release notes and a preview-channel toggle.

The browser loads one shared list from `/releases?per_page=100`. Stable selects
the most recently published non-draft, non-prerelease entry; opt-in preview also
includes prereleases. Switching channels selects from that list locally and
does not query GitHub or the router again. Reopening the popup checks cache
freshness: a fresh list causes no network request; a missing or expired list is
loaded once. After an error the same primary button offers a manual check, without
starting an installation. GitHub rate-limit backoff still applies to manual retries.
This bounded window is not a scan of every historical release. Unknown version
formats fail closed; numeric two-to-four-component tags, `-revN` and the project's
`~git<UTC timestamp>.<commit>` installed development versions are supported.
Both C and TypeScript also recognize the APK SDK's `_pre<14-digit UTC timestamp>`
installed identity. These two snapshot forms sort before the corresponding release
and are never accepted as release update targets. A
preview flag is independent of the tag format. Versions and package revisions are
compared numerically, never lexicographically. Downgrades are not offered or accepted.

Installation starts only when the user clicks Update. Closing the popup or tab
does not cancel a router job. After restart, the browser reconnects without repeated network-error toasts;
it never automatically repeats an uncertain install POST. Success requires the
new daemon's version and package revision. Reload is explicit to avoid discarding
browser drafts. The reload hint is shown only for an installation observed on the
current page and disappears after reload. The normal application authorisation
behavior is unchanged. If the confirmation GET fails, the UI keeps installation
disabled and retries the version read until it can confirm the expected version
and revision; it never retries the install POST. Reopening a page during an active
job loads local build information too. A later failure restores the candidate for
an explicit manual retry without requiring another page reload.

## Additive API

All endpoints are under `/api/v1/system`:

- `GET /update`: local build metadata only: `installed_version`,
  `installed_revision`, `asset_suffix`, `can_install`, `reason`.
- `GET /update/status`: persisted job state, or `{ "stage": "idle" }`.
- `POST /update/install`: exactly `{ "tag": "0.9.0", "release_id": 123,
  "preview": false }`. Returns HTTP 202 with `job_id` and `stage: queued`.
  Invalid/non-newer input is 400; unavailable/unauthorised installation is 403;
  an already locked job is 409; local preparation failure is 500.

Installation independently verifies a current **root** JWT, including on the Unix
socket and when general WebUI authentication is disabled. Anonymous installation
is not supported: enable WebUI authentication and sign in first. Only JSON requests
are accepted; cross-site Fetch Metadata requests are refused. Neither URLs, file
paths, package-manager arguments nor command strings are accepted from the browser.
Read endpoints do not contact GitHub and use `Cache-Control: no-store`.

Stages: `queued`, `checking`, `downloading`, `verifying`, `backing_up`, `installing`,
`restarting`, `succeeded`, `failed`, `interrupted`. A stale active state without a
live worker lock is reported as interrupted rather than permanently busy. The
handler re-reads the status while holding that lock, preserving a final success or
failure published between the original read and the lock probe. A new
manual attempt can acquire the released lock. The UI cannot show live logs while
the daemon itself is stopped; it reconnects and reads the persisted state.

## Target selection and packaging

The root build and both production SDK package templates embed `MT_BUILD_TARGET`
and `MT_PACKAGE_REVISION`, and package the independent `mt-c-updater` executable.
Changing build metadata invalidates affected objects on incremental rebuilds.
The existing target config matrix is unchanged.

Entware matches its exact build target, including `_kn`. OpenWrt matches the exact
numeric firmware release in `/etc/openwrt_release` and compiled package architecture;
it does not source that file as shell code. IPK and APK are never interchanged.
Host builds with no target, unknown firmware and rolling OpenWrt snapshots cannot
self-update. Missing exact-version assets are shown as unsupported rather than
silently selecting a nearby ABI or architecture. Existing packages lacking this
feature need a one-time ordinary package installation to gain the new API/helper.

## Independent installer and trust boundary

The API saves the current server-side config and atomically writes a request under
`<state-dir>/update`. A root-only directory, non-following file opens, and inherited
`flock` serialize jobs across daemon restarts. An independently named double-forked,
`setsid` helper performs the network and package-manager work outside the event loop.
It has a fixed executable path and clean environment; unneeded inherited descriptors
and the lock descriptor in package-manager children are closed.

The helper independently re-fetches `/releases?per_page=100` from the fixed
repository for both channels and applies the same published-at/channel policy as
the browser (not `/latest` or a shorter preview window). It verifies the release
ID/tag is still current, rejects a non-newer version, and resolves exactly
one platform asset. It requires an uploaded asset, bounded positive size, and a
SHA-256 digest. HTTPS certificate/host checks remain enabled. Redirects are bounded
and restricted to the GitHub release download hosts. Downloading is streaming to a
private temporary directory, bounded to the advertised size; actual size and SHA-256
must match before invoking the package manager. A checksum obtained over HTTPS
protects integrity against mismatched/corrupt downloads; it is **not** an independent
publisher signature and does not protect against a compromised release owner.

Space and package-manager dry-run checks precede installation. The helper copies
config and authentication state to mode-0600 backups before `opkg install` or
`apk add --allow-untrusted` on the verified local asset. The APK flag is limited to
this asset, whose integrity was checked above; no force-architecture/dependency
flags or general-purpose remote shell commands are used. New dependencies, when
required, are resolved by the device's configured package manager feeds.

Maintainer scripts may restart the daemon while the package manager still runs.
The helper waits for the command PID, not EOF from descendants that inherited its
output pipe; output is drained and the saved log is bounded to 128 KiB. There is no
arbitrary timer that kills an active package manager. After installation the helper
checks the daemon via its Unix socket and restarts through the platform init script
if needed. It only records success when the expected version/revision is serving.

## Recovery and limitations

`<state-dir>/update` retains `status.json`, `install.log`, `config.before.yaml`, and
`auth_secret.before` (when present). It retains the **last attempt**, not an unbounded
history. Downloaded package files are removed after normal completion/failure.
An abrupt power loss can leave a temporary directory requiring later manual cleanup.
Backups are not served by a public download endpoint.

IPK/APK installation is not transactional. This implementation does not promise
automatic binary rollback after a partial package installation or power loss. A
failed package manager triggers an attempt to start the service, retains diagnostics,
and reports failure. `package_installed` distinguishes a completed package command
from a later service-start failure; `false` does not prove no files were modified.
Recover a damaged installation with the matching package manager and saved config.

Before promoting a release, test on real Entware, Keenetic `_kn`, OpenWrt IPK and APK
images: successful upgrade, root/anonymous rejection, two simultaneous clients,
missing/wrong assets and digests, GitHub failure, insufficient space, config and
credentials preservation, restart/reconnect, helper surviving maintainer scripts,
page reload during update, and recovery from package-manager failure. Unit tests do
not establish device-specific service-manager behavior.

## Development backend mock

`src/frontend/dev/backend-mock.ts` registers the same three
`/api/v1/system/update` routes from `dev/update-mock.ts`. The mock reports
an intentionally old Entware/Keenetic build (`0.8.2.1`,
`entware_aarch64-3.10_kn.ipk`) to make the update button testable against
published releases; the browser still requests **real GitHub metadata**.
Read endpoints return the production response shape instead of a 404.
A simulated install accepts the development root login token, checks the
exact JSON shape and higher target version, returns 202 and progresses
through update stages based on elapsed time. A concurrent attempt returns
409, while invalid/non-newer input returns 400 and a non-root token returns
403. Completion updates only the **mock's in-memory** installed version.

The mock does **not** download packages, verify a real release, execute a
package manager, modify the filesystem or restart the router. It is only a
frontend/development contract simulator, not proof of installation safety.

## Tests

`make -C src/backend-c test` discovers the release/version tests and the subprocess
regression test. `make -C src/backend-c sanitize` applies ASan/UBSan. Frontend tests
in `tests/unit/updates.test.ts` and `tests/unit/update-mock.test.ts` run in the existing `npm run test:unit` suite and cover
version ordering, target/checksum/URL matching, response validation, caching, ETags,
rate limits, blocked browser storage and omitted GitHub credentials, plus mocked
read/install request validation, status progression and concurrent-install locks. Existing
`npm run check`, build and format checks still apply; no dependencies or workflows
were replaced for this feature.

Regression coverage for the review fixes: `test_update_release.c` and
`tests/unit/updates.test.ts` exercise shared channel selection beyond the
old 20-item window and APK SDK snapshot identities. `test_update_process.c` forces
a separate worker to publish success/failure between a status read and its lock
probe, using real file locks and atomic replacement. The test only substitutes
file ownership to run on unprivileged CI, never in production. Playwright's
`tests/e2e/updates-recovery.spec.ts` covers failed-check Retry, stale-cache popup
reopening, post-install version-read recovery and a reopened job that later fails.
