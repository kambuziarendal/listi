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
