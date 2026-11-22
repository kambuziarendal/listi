# Optional native copy benchmark

On Haiku, build each exact source tree first (same compiler, release flags and
configuration). Run this driver outside CI on a dedicated test volume with
no user data. COMMIT is an operator-supplied SHA, not a source attestation:
verify source manifests and build identities independently.

    python3 tests/performance/bench.py --volume /boot/home \
      --machine 'QEMU KVM, 2 vCPU, 2048 MiB, IDE qcow2' \
      --build BASE_SHA /boot/home/baseline --build CANDIDATE_SHA /boot/home/candidate \
      --dataset small --mode fresh --mode overwrite > timing.jsonl

The driver links the same probe against each tree's release objects, as in
the original 32-file timing. No production instrumentation is enabled.
Run without dataset/mode options for the whole matrix (one file, 32 × 4 KiB,
256 × 64 B, nested tree, 8 MiB file, all fresh/overwrite/cancel). Default:
ten repetitions per case, reversing build order on alternate rounds.
Fixture preparation, linking, verification and cleanup are outside the timed
interval. Every run uses a new synthetic tree in the selected folder, on the
same volume for both source and destination. The source is warm; neither
guest nor host caches are dropped. Successful output is read back.

Each JSON line includes wall and process CPU time, build flags, filesystem,
machine label, hrev and linker-wrapped calls from the operation thread.
Journal bytes count successful writes to the record temp FD. Directory fsync
times include inventory and destination directories. renameat does not count
BEntry::MoveTo internals or external library calls. Timings do not attribute
all time outside the measured calls to metadata. Counters are not a system
tracer and do not include worker subprocesses. Temporary-space high-water
marks, physical cache durability, NAS transfers and GUI timing are not
measured. No timing is a correctness pass threshold.
