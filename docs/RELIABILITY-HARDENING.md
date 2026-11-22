# Listi reliability hardening

## Baseline and scope

Private branch `hardening/reliability-27926` started from a clean
`audit/integrity-27917` at `734fa6996bbcc757e67b79e4e8093004fae3294d`.
The earlier 0.38.0-10 installed-package result is baseline evidence only.
It does not qualify changes on this branch.

* Recovery: ArchiveJobs already uses RecoveryInventory. Ordinary file copy
  creates a sibling .listi-copy-* in FileOperation.cpp. A filename prefix is
  not evidence of ownership. Restart inspection must not remove or replay.
* Storage: staging capacity checks already exist for parts of the network and
  archive paths. A successful preflight does not reserve space against other
  processes. Distinguish observed, estimated and enforceable limits.
* Publication: MessageStore replaces settings with renameat and FileOperation
  uses descriptor-relative unlinkat in guarded cleanup. Pinning the parent
  does not condition a directory-entry operation on the child's inode.
* Qualification: the prior native regression and installed-package tests ran
  separately. Keep their identities and results separate, and distinguish
  checks defined in Git from checks actually run or required by GitHub.

Scope excludes production, public releases, paid infrastructure and assertions
of physical-media or power-loss durability based on VM results.

## What changed

* RecoveryInventory.cpp: Reserve creates an exclusive record; Advance accepts
  only the next phase: intent, writing, ready, publishing, published. Stage
  names derive from the operation ID; a name alone never authorizes removal.
  NoteFailure is best effort when the inventory volume is full. Retire removes
  the identity-checked record and flushes the parent. ReviewRecovery is read
  only and understands the earlier archive/rename format. A matching identity
  is not a recommendation to discard the data.
* FileOperation.cpp, ArchiveJobs.cpp, PrivateScratch.h, ArjWriter.cpp,
  LhaWriter.cpp and VirtualOperation.cpp bind copy/archive/codec staging or
  record remote stage/backup names before mutation. An interruption before
  stage binding can thus leave an intent with no proven object. A lost remote
  acknowledgement can leave a valid original under the backup name. Neither
  case triggers automatic replay. A record does not certify the current
  remote bytes; inspect all named objects independently.
* StagingSpace.h: live, volume-specific headroom and accounting for concurrent
  in-process writes. Applied to local copies, previews (ImagePreview.cpp),
  editor recovery bytes (InspectorDocument.cpp), bulk rename journal writes
  (BulkRename.cpp) and flattened records/settings (MessageStore.cpp).
  Monitoring a child is not a quota. Preflight cannot reserve space against
  another process. No arbitrary per-file limit was added to ordinary copies.
* CI: qualification.yml, ci/ and the native, ephemeral and interruption
  runners require explicit case selection/results, fail on missing cases,
  record the source SHA and separate first-party native execution from
  untrusted hosted execution. See [CI setup and reproduction](CI.md).

## Publication and failure semantics

| Operation | What the implementation guarantees and where it stops |
| --- | --- |
| Regular copy replacement | Writes and syncs a distinct staged inode before renameat in a pinned directory. A successful rename switches the name, not the whole job. A later error can mean new bytes are already visible. Removable-volume flush policy differs. |
| Rename and same-volume move | Native BEntry moves an entry. A conflict check is not kernel-enforced conditional rename. Bulk rename and undo are sequences with intermediate names and journals, not transactions. |
| Cross-volume move and recursive delete | Copy and verify before guarded source removal. A stopped move can leave both copies or partial source cleanup. Per-entry checks and pinned parents do not make tree deletion atomic. |
| Trash and Restore | BEntry::MoveTo with replacement disabled; original-path attributes and the namespace move are separate operations. No intentional overwrite or merge, but no atomic no-replace guarantee against a concurrent writer. |
| Archive edit and creation | Edit syncs staged output, rechecks identities, renameat publishes relative to pinned directories, then syncs destination parent. A failed final sync is not permission to roll back. Creation uses a conflict-checked native move, not a conditional kernel rename. |
| Scratch and recovery cleanup | Checks bound identities before removal; changing an entry in the final check/unlink interval is not excluded by pinning its parent. Diagnostic scanning never deletes. |
| Editor | Document::Save edits the pinned original inode in place to preserve its identity, after syncing a recovery copy. Interrupted writes can be partial. A live failure attempts restoration only for expected bytes. This is not an atomic save. |
| Settings and inventory | SaveMessageStore uses an exclusive temp file, sync, target/stage recheck, renameat and parent sync. Failure after rename is reported separately. A final check is not compare-and-swap. |
| Remote replacement | Server renames, verification and cleanup are separate requests. A missing reply is not proof of failure. Keeping uncertain remote data is preferable to deleting the wrong original. |

fsync success here means the filesystem accepted the flush. The VM experiments
cover process death and filesystem errors, not device write-cache behavior or
mains failure. Repeating stat or adding another check was rejected as a
substitute for an identity-conditional syscall. Backup replay based solely on
a journal or on a matching filename was likewise rejected.

## Exact application qualification

Application source: 64e26b39970d2391c43da3b6446b5756ac440985, 458/458 source
manifest entries matched. Clean source archive SHA-256:
2e032cec01478623adae30e35668ef920840aebd953450b6fb9aa2865077b2a6.
Haiku R1~beta6 hrev59866+79 x86_64, GCC 13.3.0, isolated QEMU overlay.
Private Listi 0.38.0-11 HPKG SHA-256:
3412903c12c2be7ce3cb469d5eb5eb9f69bce91fe4cd13335c633dc7fa5011b4.
Installed executable SHA-256:
9329f034200578b7cdf3a2bab8fbbbe3e8c256bd9e21f9829d783aad2fee0efe.
Installed package and executable each compared byte-for-byte with the build.

* All 80 required native programs passed; built-in and installed self-tests
  each passed 58/58. Installed virtual suite passed 110/110 with SMB, FTP,
  SFTP and certificate-verified FTPS. Installed GUI: all four workspaces,
  single/dual-pane file copy and recovery review (including legacy records)
  exercised on the real app_server. Other features were regression tested,
  not individually signed off by a human in the installed GUI.
* tests/copy-crash-recovery.cpp covers ordinary-copy death around intent,
  exclusive creation, writes, verification, publication, rename and retirement,
  and injected inventory/publication ENOSPC. Source and old/new target bytes
  checked; repeated scans and substituted stages exercised. Not every operation
  has an independent process-kill test at every boundary.
* Four additional real FTP client terminations at staged, backup, published
  and cleaned boundaries preserved expected remote bytes/names. Two fresh
  review processes agreed in each case. No equivalent FTPS death matrix.
* On a real 16 MiB synthetic BFS filesystem a failed copy and two competing
  12 MiB replacements preserved the original files without staging residue.
  Staging-budget tests also exercised two stages and external child allocation
  pressure. EDQUOT on a quota-enabled filesystem was not tested.
* Original extended driver **failed** two FTP reconnect checks after sharing
  a long-lived tunnel. A separately scoped rerun of all 27 SMB/FTP/SFTP
  fault cases passed on the same installed candidate. Those results alone
  do not qualify the original driver.
* Median elapsed time for copying 32 files of 4096 bytes rose from 102.5 ms
  to 584.4 ms (three runs per build in the same VM, about 5.7x). The extra
  synchronous journal writes have measurable cost. Do not extrapolate this
  small-tree measurement to large files or physical devices.

Raw local receipts: Kambuzi reports/listi-reliability-27926, notably
final-audit.log, final-source.sha256, final-package.log, final-gui-verified.log,
installed-recovery.png, scoped/matrix.json, death-*.log, real-full-run.log,
two-jobs.log, target3.log and timing.log. No physical USB, power-cut or
real NAS testing. No production, hosted workflow or branch-rule change.
