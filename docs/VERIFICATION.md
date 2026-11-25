# Testing and verification

Listi is alpha software that works directly on files. Tests provide evidence for
specific cases, not proof that every filesystem, server or archive variant is
safe. Use disposable fixtures and independent backups.

## 0.38.0-23 alpha

The [release notes](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-23) identify the source, package digest and checks for this revision. The package is the tested binary, not a rebuild of the documentation branch. Local native tests run inside Haiku; they are not physical USB tests or hosted CI results. In particular, a successful copy to NTFS does not establish safe removal if the mounted volume still reports “device busy.”

## Folder refresh measurements (0.38.0 development)

On Haiku R1/beta6 x86_64 in QEMU, we copied 120 files of 64 KiB into an open folder on a throttled virtual USB disk. The panel started 78 and 77 directory scans in two baseline runs, versus 30 and 31 with the [refresh change](https://github.com/kambuziarendal/listi/commit/a85d258040e8d4e185c03cdf85b4465fcffed0fb). Every copied file matched its source. This counts scans, not UI latency or elapsed copy time. Separate GUI checks covered navigation away during a copy, external folder changes, cancellation and the final listing. The built-in self-test passed 58 of 58 checks.

An earlier [qualification run](POST-HARDENING-FOLLOWUP.md) measured the median time to copy 32 small files to a fresh destination: 351 ms versus 506 ms before the journal changes. That run also covered recovery after process termination. Neither test demonstrates power-loss durability or physical USB behaviour. For hosted results, check [GitHub Actions](https://github.com/kambuziarendal/listi/actions/workflows/qualification.yml) for the exact commit.

## Hands-on testing on physical hardware

Pre-release hands-on testing on Haiku identified failures that scripted checks had missed: an attributed screenshot could not be copied to a FAT stick, Inspector could identify a PNG without drawing it after discarding a text edit, and Einn inherited an oversized window. These observations motivated the revision 22 changes; they are not physical revalidation of the new package.

### Physical USB disconnect during copy

The first physical test on 0.38.0-30 ended in a kernel panic; see the [field report](#physical-usb-field-report-0380-30-2026-11-25). The steps below describe the intended checks.

1. Use disposable files and a spare USB disk. Record checksums for the source, a file already on the USB disk and an earlier completed copy.
2. Start **Copy** in Listi. Unplug the USB disk while it is still being written to, then click **Cancel**. Record whether the window responds, the job's exact status and how long it stays in that state.
3. After reconnecting and remounting the disk, record what is present at the destination and compare the three checksums. Note if a restart was necessary. Record the outcome here before changing roadmap priority 1.

## Testing in Haiku VMs

Much of the automated stress testing is LLM-assisted and runs in Haiku VMs. In the historical records below, “native” means the tests ran inside Haiku; it does not identify a bare-metal test. The commands later in this document are instructions, not additional results.

### 0.36.1-3 package results

The [candidate HPKG](https://github.com/kambuziarendal/listi/releases/tag/v0.36.1-3) was installed on Haiku R1/beta6 x86_64 in a VM (hrev not recorded). The installed executable matched the tested build. All 71 native test cases and 58 built-in checks passed. The VM record also mentions the cases below. The record does not give separate outcomes for them.

| Additional check (same package and VM) | Individual outcome |
| --- | --- |
| Full destination disk | Not individually recorded |
| Change folder contents during a copy | Not individually recorded |
| Rename and undo | Not individually recorded |
| Restore files with colliding names from Trash | Not individually recorded |

The installed app refused a move between disks. During an explicit copy, disconnecting the virtual USB device late in the write left the job pending for over 100 seconds, even after cancellation was requested. It did not report success. After cold restart, the original, an unrelated destination file and an earlier completed copy matched their expected bytes. This was a virtual USB disconnect, not the [physical USB test](#physical-usb-disconnect-during-copy).

### 0.35.0 package results

The [published HPKG](https://github.com/kambuziarendal/listi/releases/tag/v0.35.0) is the exact package tested on Haiku R1~beta6 hrev59866+79, x86_64, with GCC 13.3.0. Its tag identifies the source; the default branch includes newer documentation than the copy inside the package.

- All 71 default native audit cases passed. The build-tree self-test and the installed self-test each passed 58 of 58 checks; the executables matched byte for byte.
- The installed GUI switched through all four workspaces. A two-panel copy preserved the expected bytes. Selecting `*.txt` previewed two matches; copying the selection transferred only those two entries and left all originals intact.
- This was one disposable VM, not a physical-device pilot, an extended soak test, a same-version upgrade test or a fresh real-server network test.

## Native regression audit and smoke test

Build inside Haiku x86_64 with the dependencies listed in the
[README](../README.md#build-from-source), using a disposable test profile with Haiku's
`app_server` running. The full audit exercises optional features too: install
the p7zip native engine, LHA and ARJ tools and the package providing
`cmd:pdftoppm` for archive and preview coverage.

From a clean checkout:

```sh
make audit >native-audit.log 2>&1
```

This builds the application, links and runs all cases in the runner's required
list, and finally runs the build-tree `Listi --self-test`. Check the exit
status of **make**; a build error, missing required case or object, failed
assertion, timeout or failed self-test makes the audit fail. The log records
the selected set, compiler version, OS, system packages and Git revision and
working-tree status. Use a clean Git checkout (with Git installed) for
version-pinned evidence. An exported source tree reports its revision as
unknown. Keep the log even on failure; tests after the first failure have
not run. A case passing means the executable exited successfully; any optional
subchecks it explicitly skipped are not verified. This is not an installed-package
or network qualification.

For a quick iteration, or to rerun selected cases:

```sh
make test
make audit TESTS="selection-model search-filters"
```

`make test` builds and runs only the built-in smoke test. A focused audit still
ends with the self-test but does not qualify the full suite. To check the
failure path in a disposable checkout, request `TESTS=no-such-case`; make
must exit nonzero. To exercise a failing executable without touching any
existing assertion, create `tests/intentional-failure.cpp` containing only
`int main() { return 1; }` and select `TESTS=intentional-failure`; this
must also exit nonzero without printing a final pass.

After building, `python3 tests/run-native-audit.py [case ...]` is equivalent
to the audit step without the application rebuild. It asks the native build
for its exact target, objects, compiler, include and link flags.

For Shelf collections, run:

```sh
python3 tests/run-native-audit.py shelf shelf-collections shelf-collections-window
```

To test a restart and mount cycle, use a disposable BFS
volume and keep the test program and settings file on a different volume. With
the fixture mounted, run `shelf-collections --save-reference STORE FILE`.
Run `shelf-collections --check-reference STORE FILE ready` from a separate
process. Unmount the fixture and check for `missing`. Mount an unrelated BFS
volume with the same label and filename: the reference must still be missing
and the decoy file unchanged. Unmount it. After remounting the original on
the same device path, check for `ready` again using the current absolute file
path. Use the full path to the test binary in `build/native-audit-tests`.

Network integration tests require disposable local services and explicit fixture
setup. These are not part of the default `make audit` set. Diagnostic probes under
[`tests/probes`](../tests/probes/) have historical reproduction exit meanings and
must not be mistaken for normal passing regressions.

### Inspector, Quick view and native source editing

After building on Haiku, run the inspector tests against the linked native
objects. PDF page tests require `pdftoppm`; media support depends on the codecs
and audio output present on the test machine.

```sh
python3 tests/run-native-audit.py inspector-content inspector-document inspector-editor inspector-photo
```

Also test the installed app with disposable files:

1. In each workspace select a source and use **Listi > Quick view**. In a
   two-panel workspace try both active panels. Change the selection, then
   toggle Quick view off. Both original paths and selections should survive;
   Space must still select. A permanent Inspector and a temporary Quick view
   should not change one another's saved settings.
2. Preview a script, HTML and Markdown. None should execute. Choose **Edit**,
   change the draft, then select another file in the main window. Try both the
   main window's Quit and app-level Quit: an unsaved draft must stay open.
   Exercise Find/Replace, undo/redo, go to line, wrap, Save and Cancel. Check
   the original bytes, line endings and a typed BFS attribute after saving.
   Try a read-only file, random bytes, a truncated preview and an unknown
   extension. A rejected edit must not change the source.
3. While a draft is open, change the source externally or replace the entry
   with a different file at the same path. A failed save must preserve the
   other file and the draft. The document tests also inject sync failures and
   check restoration or retention of the original-byte recovery file.
4. Inspect an image at fit and at actual size, then zoom and drag. Try a JPEG
   with known camera and GPS attributes. Coordinates must not appear until
   requested. Switch quickly between an image, a PDF and text; late replies
   must not replace the current preview.
5. Select audio/video without starting playback, play and stop an inline sample,
   and open a local clip in **MediaPlayer**. Try an unavailable codec or audio
   device separately. Seeing a frame is not evidence of audible output.

### Tvískipt and Smiðja function buttons

With both panes open, select files and toggle **View > Show function buttons**.
Check that the bank hides and returns without changing the window frame, active
pane, selection, or command field, with the bank at the top and at the bottom.
Hide the bank, make the window too small for it, and enable **View > Show function buttons** again.
Check that configured actions remain available in **View > Function actions** without enlarging
the frame. Restart Listi to check that the toggle is remembered independently
of Smiðja. Run the `preferences-state` native test for settings compatibility.

### Flat View

Run `python3 tests/run-native-audit.py flat-view file-search live-search shelf`.
The Flat View fixture scans local files with duplicate
names and a directory-link cycle. It injects an unreadable branch, exercises
filtering and frozen Shelf selection, and stops a deliberately paused scan.
For a manual workspace check, select files in either panel, set a panel filter,
and choose **View > File view > Flat View**. Confirm that results appear in
that panel without opening a window, and the opposite panel remains usable.
In the installed app, include duplicate names in two subfolders, an extensionless
README, mixed-case extensions and files of different dates and sizes (e.g. 2
and 10 bytes). Click each of the five headings twice. Check both directions,
path-based ties and the extensionless group. Select one duplicate before
sorting, then copy its full path to confirm the selection still targets it.
Choose the original view in the panel selector and check that its selection and filter are restored.

### BFS attribute columns

Run `python3 tests/run-native-audit.py attribute-columns attribute-columns-ui file-properties properties-faults`.
The UI fixture opens Properties and two panes looking at the same folder. It
checks stored, empty and missing values, adds a second column without touching
an open edit draft, then sorts and clears the columns. Clearing restores the
original pane view without changing the other pane. The separate attribute
test creates and removes its own disposable BFS index.

### File operation fault and job-result tests

On Haiku, run `python3 tests/run-native-audit.py file-operation-faults operation-queue bulk-rename`
after building. These tests use disposable entries to check staged output after
short/interrupted writes, permission errors during writing or source removal,
partial batches and job results when UI notifications are delayed or lost.
To check the interval before an operation is queued, also run
`main-window-routing selection-model file-drop`. The fixtures keep a scanned
row selected while replacing the entry at that pathname. File actions must
refuse the replacement without clearing the preceding clipboard.

Injected syscall failures also run when normal permission bits alone would not
prevent the test user from writing.

### Bulk rename and undo

In the bulk rename preview, select a row, enter its exact new leaf name and
click **Set name**. To swap two names, do this for both rows, then apply the
checked preview. **Use rules** removes a row override. With the queue idle,
**Files → Undo last bulk rename** checks the names, entry identities and
contents again before moving anything. Undo is session-only, covers the latest
successful batch, and does not overwrite an occupied original name. An
unreadable, unusually deep or large batch can rename without undo; undo
verification is limited to 512 MiB and 200,000 entries per batch.

The native regression injects failures after staging, checks rollback and verifies
that an entry blocking recovery is retained. If rollback cannot finish, the
operation report lists verified locations (including hidden
`.listi-rename-*` names) or says when an entry could not be located. Inspect
these paths before retrying. An interrupted process may leave a hidden
`.listi-recovery-rename-*` review record in the affected folder. Open **Listi → Review interrupted
operations…** to see the journal path and check its identity before reading
it. It records
the frozen folder, planned names, entry identities and each flushed rename
intent, including rollback. An intent does not prove that a rename took
place. Show hidden files, inspect the original, target and temporary names,
and compare their current filesystem identities with the record. If an
identity is absent, ambiguous, or on a remounted/reassigned device, do not
guess. Keep the record and entries until the correct mapping is confirmed.
Listi never replays the record or deletes unrelated entries on restart.
A successful filesystem flush is not a guarantee against power loss.

Haiku does not provide atomic no-replace or identity-conditional rename.
Listi pins the parent directory and checks the source and target immediately
before each step, but an unrelated writer changing either name between the
checks and the kernel call can defeat the guard. Do not run a batch while another
application is renaming entries in the same folder.

### Moving between volumes

A local Move across filesystem devices copies each verified root, then retains
the original in its source volume's Trash. Unsupported source Trash and
interrupted transfers must leave the source untouched. Exercise regular files
and directory trees, and separately test archive-member moves, Copy between the
same two volumes and same-volume Move. Removable regular-file streaming can
withdraw from blocked reads, writes and flushes. Filesystem metadata and
directory enumeration calls do not have the same cancellation guarantee.

The opt-in native test uses a writable volume distinct from `/tmp`:

```sh
LISTI_TEST_VOLUME=/path/to/test-volume python3 tests/run-native-audit.py cross-volume-move
```

Use a dedicated disposable volume for disk-full checks. Never fill an
ordinary working volume to exercise a failure path.

### Recovering a staging directory

A failed archive operation reports if Listi cannot remove its staging folder.
On restart, **Listi → Review interrupted operations…** reads Listi's
private stage and bulk rename inventory without probing affected volumes.
**Check local paths** optionally checks whether an entry and its parent still
match. The review can be closed while a check is pending. It never resumes, relocates or
deletes an entry. A missing or substituted name is not safe to reuse.
A match is only advisory, especially after devices are remounted.
In the destination you can also look for hidden `.Listi-archive-stage-*` directories
in the affected destination. The name alone does not establish ownership.
Inspect the contents and compare them with the source and destination before
removing anything. Listi does not delete staging-looking names on restart.
Only identities belonging to live operations are excluded from listings and
search. Regular-file staging can leave `.listi-copy-*` files when driver I/O
remains pending after cancellation; check them after reconnecting.

### Staging capacity

Network and archive spools, decoded 7z data, private archive copies and
Listi-controlled writer outputs check the live available capacity of the
backing volume before growing staged files. Multiple temporary files on the
same volume draw on the same available space. The reserve is 0.25% of volume
capacity, at least one volume block and at most 16 MiB. Haiku growth
checks also leave a 256 KiB allowance for filesystem preallocation and
extent metadata. Known transfer
lengths also bound actual spooled bytes. There is no fixed maximum file size
introduced by the reserve.

The external LHA and ARJ codecs own their scratch writes. Listi checks the
shared reserve before launch, while waiting and before accepting output, and
stops the codec if space runs low. Haiku does not provide the per-child file
size resource limit needed to guard each external write. Other processes
and buffered filesystem metadata may also allocate between checks. This is
not a global filesystem quota or a guarantee against every full-volume
event.

The opt-in `staging-budget` test requires a disposable BFS volume of at most
128 MiB, separate from `/tmp`. It writes real data into two files until the
shared reserve rejects more bytes. Never point it at a working volume.

## Native release evidence

Run `make audit` on Haiku in a clean checkout. An opt-in local alternative
uses a disposable QEMU overlay of an existing Haiku image with SSH, compiler
and Listi build dependencies already installed:

```sh
python3 tests/run-ephemeral-audit.py --base /path/to/haiku.qcow2 \
  --key /path/to/private-test-key --output /path/to/new-audit-output
```

The runner exports and verifies the committed source, records logs, uses
loopback-only SSH and discards the overlay. `--preflight` verifies boot and
export without compilation. It does not configure hosted CI. Run
`make audit-sanitize fuzz-parsers` separately on a POSIX host with ASan/UBSan
support for the portable transfer fault suite and seeded URL/digest tests.

 Save the complete native log and
process status. For an exported tree, pair the log with the exported commit
and a verified file manifest, since Git metadata is not included. The runner
reports the source revision when available, executable digest, compiler,
selected cases and package inventory. Check fixture output for explicit skips,
particularly when a case needs a second volume or optional archive tooling.
Run the opt-in network suite separately against disposable servers for each
protocol you intend to qualify.

Build the package separately, record the HPKG digest, install it on the test
system, confirm the installed executable digest and repeat the relevant
app self-tests and UI operations against that executable. QEMU exercise with
USB hot-unplug is not a physical test. Record physical combinations separately,
including clean eject, disconnect, remount and content comparison. A passing
source audit alone does not qualify an installed artifact, another architecture,
another filesystem, or physical device behavior.

## UI and package checks

Automated tests do not replace installed-package checks in Haiku. Relevant
workflows should also be exercised through the interface, including conflicts,
cancellation, compact windows, light/dark appearance, workspace switching and
settings compatibility. A source-built executable and an installed package are
separate artifacts and should be identified separately.

## Verification limits

- A test source or a previous successful run is not a fresh qualification of the
  current checkout. Record the commit, binary and environment for each run.
- Results do not cover every removable device, filesystem, NAS, server or WAN
  condition. Archive capabilities depend on installed engines.
- Concurrent edits, partial moves and power loss require special care; Listi is
  not a transactional backup system and does not promise universal undo.
- Performance claims need documented hardware, datasets and cache conditions.

For behaviour and known limits, see the [workflow guide](GUIDE.md).

## Reporting a problem

[Open an issue](https://github.com/kambuziarendal/listi/issues) with the Listi and
Haiku versions, exact reproduction steps, error messages and whether files were
changed or left in a partial state. Prefer a minimal synthetic example. Do not
include passwords, private filenames or private file contents in a public report.

For the optional host-driven network interruption matrix, set
`LISTI_NETWORK_TEST_BINARY` to the absolute path of the freshly linked
`network-interruptions` test in the Haiku guest. Set
`LISTI_NETWORK_KNOWN_HOSTS` to the guest's known-hosts file for the disposable
SFTP fixture. This avoids testing a binary left by an older checkout.

### Folder history through a store disconnect

In a disposable Haiku guest, configure a test source, save at least two versions
and confirm the source pane shows their count. With the history browser closed,
temporarily take only the test store offline. Leave and revisit the source
without restarting Listi: the badge must say unavailable, not zero versions.
Open it to check the explanation. Return the same store, navigate to an
unconfigured folder and back in the same window: its count must recover without
switching applications. In the neighbouring preview, step to the older version
and back; closing it must return the previous folder without writing either one.

### Physical USB field report: 0.38.0-30, 2026-11-25

The tester unplugged the stick during Copy and dismissed the visible errors. The file remained red with “Needs attention”. Screenshots in the open folder could still be viewed. Attempting to open another folder on the disconnected stick was followed by a Haiku kernel panic. The photographed trace shows a directory reader, Listi directory scan frames and the FAT filesystem path. The root cause and exact FAT variant are not established. No post-remount checksums or BFS outcome were reported. Physical disconnect qualification failed; beta remains on hold pending a fix and hardware verification.
