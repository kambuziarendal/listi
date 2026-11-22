# Listi post-hardening qualification

## Starting point

The application baseline is 64e26b39970d2391c43da3b6446b5756ac440985
(0.38.0-11). The corrected extended driver at
2def86485af0d13265e39a7ade2a8f3a1147d602 is its descendant. The development
branch starts at a969d2c, which adds the driver receipts. Between the
application baseline and a969d2c only ci/run-extended.py,
tests/run-network-interruptions.py and docs/RELIABILITY-HARDENING.md differ.
The older followup/performance-27934 checkout had uncommitted runner work;
it was not reset or used as the application baseline.

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
