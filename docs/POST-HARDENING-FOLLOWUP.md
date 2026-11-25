# Listi post-hardening qualification

## Starting point

The application baseline is 64e26b39970d2391c43da3b6446b5756ac440985
(0.38.0-11). The corrected extended driver at
2def86485af0d13265e39a7ade2a8f3a1147d602 is its descendant. The development
branch starts at a969d2c, which adds the driver receipts. Between the
application baseline and a969d2c only ci/run-extended.py,
tests/run-network-interruptions.py and docs/RELIABILITY-HARDENING.md differ.

The historical warm-source, fresh-directory timings in timing.log, in execution
order (microseconds), are 123051, 102490, 101088 on revision 10 and 571349,
584356, 604394 on revision 11. These are three-run observations, not a
performance threshold. They contain no overwrite controls or syscall timing.

## Safety boundaries and field qualification

A pinned parent descriptor protects against acting relative to a newly resolved
parent. Checking the leaf before rename/unlink is not a kernel-enforced
identity-conditional operation. A failed flush after namespace publication is
not a rollback. In-place editor saves preserve inode identity and sync a recovery
copy before editing; they can leave partial writes. Editor snapshots and old
rename records are not converted or replayed. Review must not repair or delete
a stage based on a filename, a matching identity or an apparent phase.

The storage guard samples volume space and accounts for writes admitted by
this process. It cannot reserve space against another process. Sampling an
external codec does not bound its peak allocations. The quota regression
injects EDQUOT into the ordinary staged write after EINTR and a successful
short write. It checks the reported error, zero completed moves, unchanged
source and original target, and absence of a staged sibling. This is not a
quota-enabled filesystem experiment.

### Disposable hardware checklist (not a VM acceptance test)

1. Use a dedicated test machine with no user disks or network shares mounted.
   Identify the scratch drive by serial, its volume by mount point, and the
   test NAS by an isolated share and dedicated account. Stop if any identity
   is ambiguous. No formatting of an existing volume is required. For a power
   test use a sacrificial boot installation as well as a scratch destination.
2. Record Haiku hrev, installed package and executable SHA-256, drive/controller,
   filesystem and NAS/server versions, protocol and write-cache settings. On
   separate storage retain hashes of deterministic source bytes, the old target
   and an unrelated sentinel. Start with both fresh and overwrite fixtures.
3. Perform one uninterrupted copy as a control. Repeat with the USB device
   disconnected during staging, then in a separate run around publication.
   For NAS interrupt only the disposable share connection during transfer and
   around rename/acknowledgement. Repeat controls after reconnection. Record
   the observed phase; do not infer a precise boundary from a progress bar.
4. On the dedicated power fixture only, interrupt power during staging and
   separately near publication. Before repair, replay or retry, capture the
   remounted filesystem and record which volume(s) lost power. If mount or
   filesystem recovery fails, record that outcome rather than repairing the
   only copy of the evidence.
5. Inventory source, old/new target, stages, backups and sentinel. Hash what
   exists. Inspect Recovery Review twice before any retry; both reviews must
   leave names and bytes unchanged. Record UI outcome and errors separately
   from observed bytes. Never treat ambiguity as permission to clean up.
   Keep uncertain data and logs. Only retry in a fresh fixture. A missing
   source/sentinel, silent replacement of unrelated data, or claimed success
   with incomplete output fails qualification.

## Performance: measurable improvement, target missed

All 550 individual observations, including wall time, CPU, bytes and syscall
counters, are in [the CSV](benchmarks/copy-followup.csv). See the
[benchmark protocol](../tests/performance/README.md) for reproduction and
counter coverage. Ten repetitions per build and case, alternating execution
order, same release compiler (GCC 13.3), same warm-source policy, same BFS
guest (hrev59866+79, QEMU KVM, 2 vCPU, 2048 MiB). This measures the native
copy operation linked against each build's release objects, not a GUI gesture.
The exact application revisions were 734fa69, 64e26b3 and eacb4c3. The final
qualification pin bdce5f3 has identical application code to eacb4c3. The first
attempt to run the historical comparison produced no samples: a Python import
collision in the probe directory stopped argument parsing. Only completed runs
are included. No arbitrary timing threshold is enforced.

| Series | Dataset | Destination | -10 ms | -11 ms | Final ms |
|---|---|---|---:|---:|---:|
| before | small | fresh | 92.08 | 580.81 | n/a |
| before | small | overwrite | 83.20 | 520.27 | n/a |
| final | small | fresh | 77.20 | 506.46 | 351.46 |
| final | small | overwrite | 85.53 | 541.57 | 354.66 |
| final | small | cancel | 3.72 | 11.97 | 12.34 |
| final | one | fresh | 2.97 | 17.29 | 12.18 |
| final | one | overwrite | 3.07 | 17.04 | 10.85 |
| final | one | cancel | 3.85 | 12.38 | 12.22 |
| final | nested | fresh | 79.66 | 537.44 | 359.98 |
| final | nested | overwrite | 88.45 | 553.58 | 357.96 |
| final | nested | cancel | 4.49 | 14.28 | 14.51 |
| final | medium | fresh | 139.85 | 150.49 | 158.14 |
| final | medium | overwrite | 145.93 | 151.70 | 154.16 |
| final | medium | cancel | 4.17 | 13.47 | 14.36 |
| final | tiny | fresh | 731.50 | 4935.94 | 3511.82 |
| final | tiny | overwrite | 763.03 | 4753.54 | 3401.48 |
| final | tiny | cancel | 4.51 | 14.52 | 15.10 |
| simulated-usb | small | fresh | 249.55 | 726.42 | 544.08 |
| simulated-usb | small | overwrite | 250.39 | 720.97 | 525.37 |

Final series, fresh 32 × 4 KiB, medians (times in ms; overlapping summaries
of different runs are not additive):

| Revision | Wall | CPU | Journal bytes | Journal fsyncs / ms | Directory fsyncs / ms | Data fsyncs / ms | renameat calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| 734fa69 | 77.2 | 17.3 | 0 | 0 / 0.0 | 32 / 6.6 | 32 / 61.2 | 0 |
| 64e26b3 | 506.5 | 126.4 | 104828 | 160 / 305.0 | 224 / 86.6 | 32 / 60.5 | 160 |
| eacb4c3 | 351.5 | 91.1 | 61412 | 96 / 188.4 | 160 / 59.9 | 32 / 61.8 | 96 |

The old five record replacements per file, their directory flushes and removal
are the main measured difference; the time is not predominantly CPU. Removing
two redundant replacements per file reduced fresh median latency by 31% and
overwrite by 35% relative to revision 11. The fresh median still exceeds the
matched revision 10 by 4.55×, and the historical 153.75 ms stretch target by
2.29×. Medium fresh copies regressed by about 5% compared with revision 11;
cancellation did not improve consistently. Do not generalize the small-file
speedup to sequential I/O. No NAS performance, temp-space high-water mark,
cold-cache, physical USB or power-loss conclusion follows from these tests.

## Durability contract and tests

Intent must be durable before exclusive creation; Writing must bind the stage
identity before writing data. Local copies still verify and sync data before
persisting Publishing. Only then can the checked rename occur. The destination
directory must be synced before checked retirement and its directory sync.
Writing excludes publication, whereas Publishing permits either outcome,
including an already durable rename. A failure after publication is not
success. Nothing in review authorizes deletion or replay. This contract was
recorded before implementation (perf/invariant-before-code.txt). There is no
cross-file batching, ownership by prefix, new journal format or removed flush
from a retained transition. The separate remote path retains all five phases.

The copy-crash-recovery suite terminates the copying process before and after
publication-intent record replacement, after destination rename, after directory
sync, and at record removal. It also interrupts the earlier stages and injects
ENOSPC at initial record creation, preparation, publication and retirement.
It checks bytes, outstanding record count, repeated read-only review and
substituted stages. This is process termination, not a simulated drive cache
loss. We did not merge the editor or legacy rename journals or batch distinct
files: no tested multi-object crash contract justified that change.

## GitHub activation and ownership

Read-only GitHub API snapshot: public default branch haiku-native at 255a06f,
Actions enabled, zero workflows and zero runs, protection endpoint 404,
protected=false, empty rulesets. There is no successful hosted run ID or
required check. We did not publish the private candidate to this public repo
or change repository settings. The workflow targets PRs and pushes to
haiku-native and manual dispatch, uses pinned actions, bounded standard
hosted runners, read-only contents and no persistent checkout credential.
No privileged self-hosted PR runner. Local actionlint 1.7.7 passed. The
[owner procedure](CI.md#enabling-public-ci-without-paid-services) separates
the first green Portable and Haiku native runs, disposable public fixture
requirements, manual native verification and an actual refused merge. Do not
make a missing or skipped check mandatory.

## Qualification history and negative evidence

The first complete application qualification used bdce5f3. All 464 archived
files verified in the guest, 80/80 native programs, 58/58 installed self-tests,
110/110 installed protocol checks (including certificate-verified FTPS), and
27/27 SMB/FTP/SFTP interruption cases passed on the seeded fixture. GUI checks
included all four workspaces, a read-only recovery window, conflict abort and
replace, cancellation during transfer and a byte-exact 36 MiB retry. The first
conflict OCR attempt stopped before clicking because the mouse pointer obscured
the label; the retry verified the label and file contents. The first VM
preflight stopped before build because the scratch disk was not mounted; the
operator preserved the seeded disk and mounted a new disposable BFS volume.

**Storage negative control:** the unchanged extended test failed on the fresh
96 MiB BFS even though it passed on the original seeded fixture. Closing two
file descriptors released preallocated extents: 237 free blocks became 2,572,
and the writer completed with 523 still free (target 122). That was a bad
test premise, not an undetected exhaustion. Commit e17d847 keeps the
descriptors open through that probe without changing an assertion or product
code. The diagnostic codec could consume the remaining free blocks before
sampling caught it. On a separately prefilled disk the controlled writer
stopped but only 117 blocks were available after fsync against the nominal
122. This negative result remains: we do not guarantee a hard post-flush
reserve. Two competing 12 MiB replacements on a 16 MiB BFS preserved both
originals and sources, and left no stages. Quota coverage is injected EDQUOT,
not a real quota mount.

The initial pinned binary SHA-256 was
`58866d4847bd016ea89df3a6401aa78bd7bd4fbb8eb682eb4dd70a3003d62024`
(identical to the measured binary); its private HPKG was
`fa03488fe9b18f683dced23f821010eb04468019d076c7c879caef4e50d9c257`.

## Final pinned result

Clean native source **e17d847cf85c53a4728e729846bdf3296d364c96**, 464/464
manifest entries matched, source tar SHA-256
`8b64b999b7275fe625d2d709424e3788bbc0e7609e6ce7f9e689de85bd72f526`.
On Haiku hrev59866+79 with GCC 13.3 the installed executable matched the
benchmark executable byte-for-byte. Installed executable SHA-256:
`58866d4847bd016ea89df3a6401aa78bd7bd4fbb8eb682eb4dd70a3003d62024`.
Final private 0.38.0-11 HPKG SHA-256:
`8310b3d4f7c31f8764c0a56920fe9b7d147624a3a25bdef7806efb4d15716f7b`.

Default native programs 80/80, installed self-test 58/58, installed protocols
110/110, SMB/FTP/SFTP interruptions 27/27. No required case skipped on the
successful run. FTPS is included in the 110, not the 27. The corrected storage
fixture passed on fresh and seeded BFS. Full-disk and competing-job cases passed.
Installed GUI passed all four workspaces, ordinary and clipboard copy, conflict
Abort/Replace, recovery review, and in-flight cancellation followed by a
byte-exact 36 MiB retry. The final GUI did not require a retry of its driver.

The final extended driver required three attempts: the first stalled at SMB
startup after 54 checks and independent guest SSH timed out. The disposable
VM was rebooted, trust restored and installed hashes rechecked. The second
refused to overwrite a surviving fixture directory. Its contents were retained
under a different name. The third run exited zero. These are not three green
runs; the first stall is not root-caused. Trust restoration and shutdown
were verified.

Linux GCC and Clang each passed the portable checks on the pinned source,
including compiler/assertion/missing-case negative controls, 24 transfer fault
checks under ASan/UBSan and 25,000 deterministic inputs. The first local Clang
attempt lacked clang++; the successful local run used extracted Debian Clang
19.1.7 and its sanitizer runtime. Actionlint passed. None is a hosted run.
No release, push, branch-policy change or production deployment.

| Area | Classification | Evidence and boundary |
|---|---|---|
| Performance | improved with limits | 550 observations; fresh 32-file median 506.46 to 351.46 ms; target missed. |
| Hosted CI | implemented but not activated | Workflow lint and local gate passed. No GitHub run. |
| Native CI | implemented but not activated | Trusted local qualification verified; no hosted native check. |
| Branch protection | not done | API confirmed unprotected; no ruleset or enforcement test. |
| Crash recovery | improved with limits | Process-death and error boundaries passed; old journals stay read-only; no power-loss qualification. |
| Storage safety | improved with limits | Two BFS topologies, ENOSPC, injected quota rejection and competing jobs; no guaranteed post-flush reserve or external codec cap. |
| Atomicity | improved with limits | Pinned-parent and stale/substituted identity checks retained; no identity-conditional leaf rename/unlink or atomic in-place editor save. |
