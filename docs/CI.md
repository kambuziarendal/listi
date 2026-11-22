# Qualification gates

Configuration is not a recorded green check. The hosted workflow and required
branch checks are **not enabled or verified by this change**. A local Haiku
audit does not prove that GitHub Actions, the extended driver or physical
hardware passed. Keep the outcome for each separate lane.

## Portable (Linux)

In a clean checkout with Git, Bash, Python 3, Make and a C++17 compiler with
ASan/UBSan, run `bash ci/portable.sh BASE`, replacing BASE with the commit on
which the change is based. This checks changed-line whitespace, Python syntax,
the native test inventory, deliberate runner failures, the POSIX transfer
fault suite and seeded parser/digest tests. On GitHub a PR tests the merge SHA
against its base SHA. Logs record the checkout SHA, not just a branch name.
For changed C++ lines also see the clang-format review in CONTRIBUTING.md.

The gate-contract check runs the actual native runner with a tiny substitute
build tree on Linux. It asserts that a positive control exits zero, a compiler
error, failed assertion and missing case all exit nonzero, and none of the
three negative runs prints a final pass. This is a runner check, not a Haiku
application build. New top-level C++ tests must be in the native suite or
explicitly classified as special/optional.

## Native (Haiku x86_64 in a disposable overlay)

Use a clean checkout, Python 3, QEMU and OpenSSH on a disposable Linux host.
Never use an unreviewed checkout on a persistent workstation or runner: the
orchestration scripts themselves run on the host. The public CI workflow uses
GitHub-hosted throwaway runners only, never a persistent self-hosted runner,
never pull_request_target. No deployment rights, write tokens, secrets,
agent forwarding, caches or reusable host credentials are needed.

Prepare a powered-off **test-only** standalone qcow2 image (no backing file)
with a fresh Haiku x86_64 installation, GUI app_server, OpenSSH, GCC, Python
3, Make, Haiku build tools, development headers and required optional codecs.
See [verification](VERIFICATION.md#native-regression-audit-and-smoke-test).
Enable SSH for account `user` with a dedicated key pair for this image only.
The base should not contain Listi, prior checkouts, personal credentials,
host mounts or user data. Verify locally before using it as a public fixture:

```sh
test -z "$(git status --porcelain)" && \
python3 tests/run-ephemeral-audit.py --base /path/to/haiku.qcow2 \
  --key /path/to/fixture-only-key --output /path/to/new-evidence --qualify-package
```

This exports committed source, compares the source-file SHA-256 manifest in
the guest, builds and runs the full native audit with its built-in self-test,
rejects explicit skipped cases, builds a new HPKG, installs it *in the overlay*,
compares both installed package and executable byte-for-byte with the build,
and reruns the installed self-test. Keep source.txt (host, QEMU, commit and source archive digest), source.sha256,
verify.log, audit.log, installed.log and result.txt. The wrapper requires one
completion per selected program, rejects failed/skipped receipts and checks the
built and installed self-test counts against the PASS lines (at least 58 each).
A missing, incomplete, failed, focused or preflight result is not an installed
package pass. Preserve the process exit status as well as those logs. A timed-out GitHub job is not green. For local automation wrap
the command in an appropriate process-group timeout (e.g. GNU timeout 150m).
Focused or preflight-only runs are not accepted for package qualification.
No release assets are uploaded by the workflow; only short-lived logs are
requested. Power-loss and physical USB behavior are not tested here.

## Extended (explicitly configured disposable environment only)

Prepare a test-only image additionally containing a writable, disposable BFS
volume mounted at /ListiTest (at most 128 MiB, distinct from /tmp, e.g. a
small second partition of the overlay-backed guest disk). Include the private
test CA's **public certificate** in the trust store actually used by Haiku
curl; no TLS bypass. Use a test server certificate for IP SAN 127.0.0.1 signed
by that CA. Store the server cert and key on the isolated host as ftps.pem and
ftps.key in an owner-only fixture directory. Do not use a real CA or a real
service key. The production trust stores are not accessed or modified.

Use a fresh Linux runner or isolated network namespace with dedicated free
loopback ports 1445, 2121, 2143, 2222, 23990, 31000 to 31019 and 31300 to
31309. Install the Python fixture dependencies in a test virtual environment
(impacket 0.13.1, paramiko 5.1.0, pyftpdlib 2.2.0 and pyOpenSSL 26.4.0).
Allow SSH remote TCP forwarding *in the disposable guest only*. From that
Python environment:

```sh
python3 tests/run-ephemeral-audit.py --base /path/to/test-image.qcow2 \
  --key /path/to/fixture-only-key --extended-fixture /path/to/test-tls-directory \
  --output /path/to/new-extended-evidence
```

This requests the same full source and installed-package audit, runs the
staging-budget and cross-volume tests, builds the interruption client from
the same objects, generates synthetic archive fixtures and runs the installed
virtual test with SMB, FTP, SFTP and certificate-verified FTPS over loopback.
It then requires the full 27-case SMB/FTP/SFTP interruption matrix, including
server-side checks. FTPS is not part of that nine-fault-per-protocol matrix.
Recovery and filesystem fault injection cases are in the full native suite.
Data files, shares, configured keys and raw images are not Actions artifacts.
Use a dedicated host; the legacy fixture has fixed ports. The extended lane
is **not** a required hosted check and its absence does not qualify it.

## Enabling public CI without paid services

1. Use a **public** GitHub repository with free standard Linux-hosted runners
   and available Actions artifact allowance. Do not enable paid usage. The
   jobs are deliberately disabled in private repos; a skipped job there must
   not be required as proof of verification.
2. Have the owner make the public, sanitized test image and its disposable
   guest-only SSH identity available at stable HTTPS URLs on existing free
   infrastructure. Both are **public fixtures**, not secrets; this identity
   must authorize nothing outside that image. Compute SHA-256 independently
   and add repository Actions **variables** LISTI_CI_IMAGE_URL,
   LISTI_CI_IMAGE_SHA256, LISTI_CI_IDENTITY_URL and LISTI_CI_IDENTITY_SHA256.
   Missing variables, failed downloads or checksum mismatches fail native CI.
3. After review and authorization, land the workflow on the default branch.
   Enable Actions and read-only token access. Run a manual dispatch and
   a test PR; check the exact commit, artifact logs and stable names
   **Portable** and **Haiku native**. Both checks must actually complete.
4. If native fixtures are unavailable, qualify **Portable** alone after its
   own first green PR check; run the native command above on the *reviewed PR
   merge SHA* on a trusted disposable host before approving. Record the commit,
   evidence path and command exit in the review. This is manual review evidence,
   **not** a GitHub status; do not require **Haiku native** until the fixtures and
   a real green native check exist. Use no self-hosted runner for untrusted code.
5. Settings → Rules → Rulesets: create an active branch ruleset targeting
   the default branch. Require PRs and status checks before merge, select
   the verified check name(s) from *GitHub Actions*, require current branches (or test
   the merge queue before enabling one), and limit bypasses. Require review
   for workflow, qualification script and required-test list changes. Verify
   an intentionally failing throwaway PR cannot merge, without merging it.
   Neither repository administration nor hosted execution is performed by a
   local test run. If infrastructure or permissions are missing, leave that
   gate explicitly unqualified.
