# Changelog

## Unreleased

- Local copy and move check whether a target would overwrite another selected source, including across the copy and move parts of a Tracker paste. Skip All no longer rescans destination names for each skipped file.
- Folder history reports unreadable saved children or attributes instead of restoring only a prefix. Shelf collections, saved places, searches and working sets reject incomplete saves.
- Directory listings and Flat View check how many rows were delivered before reporting a complete scan.

## 0.38.0-48 (private alpha candidate)

- Replace All cannot replace a result delivered earlier in the same transfer. A directory alias with a different spelling is not an implicit merge, including on a later attempt.
- Private network folders download into restricted local directories. Job Details distinguishes newly created folders from existing ones.
- Permanent deletion accounts for link-count changes caused by its own removals. Incomplete job confirmations are rejected rather than submitting a smaller selection.
- SHA-256 comparison uses an exact FTP lookup when available without dropping its independent final directory check.

## 0.38.0-47 (private alpha candidate)

- Local transfers refuse to merge distinct source directories into one destination directory. A move into an existing directory checks access permissions and ownership before copying its children.
- When a new name is already visible but the directory flush fails, Job Details reports that distinction. New folders, links, and same-volume moves use this check.
- Network jobs recheck the selected source before rename or move. Upload receipts distinguish an acknowledged transfer from publication at the final name.
- Local listings keep readable rows and identify incomplete scans. Saved places and searches reload the observed list after replacement even if its directory flush failed.
- TAR edits retain available access, change and creation timestamps. Cleanup after moving many separate files into an archive checks each selected path and its ancestors rather than rescanning all selections.

## 0.38.0-46 (private alpha candidate)

- FTP folder receipts and SHA-256 comparisons describe visible entries only. Direct FTP rename/move is refused because the protocol lacks a no-replace guarantee. Uncertain upload stages and backups remain identifiable for recovery.
- Directory copies check for aliases between different source folders. An SFTP directory listing alone cannot establish that a name is unused.
- TAR moves verify local numeric ownership before removing the original; TAR edits preserve member mode and fractional modification time, and reject unsupported extended metadata.
- A move that would break a hard link by copying and deleting now stops before replacing the target. Copy-and-delete moves also check ownership and special mode bits.

## 0.38.0-45 (private alpha candidate)

- FTP operations distinguish an entry that is absent from one the server may have hidden. An incomplete listing cannot authorize a rename, certify a folder download, or hide a possible recovery file.
- Folder comparison and single-file transfers check the server's file identity when available, even if the size and modification time match.
- TAR-to-TAR moves preserve member ownership. Archive edits retain empty typed BFS attributes on the archive itself.
- CSV exports distinguish a known Unix epoch from an unknown modification time.

## 0.38.0-44 (private alpha candidate)

- Permanent network deletion checks the selected type and available revision information before removing anything from that selection. If a folder deletion stops partway, the report identifies entries already removed.
- Folder comparison still verifies visible file pairs by SHA-256, but no longer certifies an entire FTP tree when the server falls back to LIST. Uploads recheck each source directory before reporting it complete; this is not a filesystem snapshot.
- Moving a regular archive member to an unused local name now claims that name exclusively. TAR edits preserve and check existing member ownership before replacing the archive.
- Folder history restores empty typed BFS attributes and verifies the type and size before removing the incomplete-copy marker.

## 0.38.0-43 (private alpha candidate)

- When copying a regular file to an unused name, Listi claims that name exclusively. This also protects Duplicate and the copy step of a cross-volume move. The verified stage stays until the new file is flushed, so the destination needs space for both copies. Direct rename, same-volume move, symbolic links and publication of an extracted folder still have separate collision limitations.
- Search and Flat View report skipped entries on known unavailable paths rather than reporting a complete scan.
- Clipboard saves reject destinations that became unavailable after the name prompt opened. A rejected paste job no longer leaves Cut reserved while waiting for a job that never started.
- TAR creation handles long UTF-8 member names in a worker using the C locale. New archive creation checks the output against the file actually written before publishing it.

## 0.38.0-42 (private alpha candidate)

- Folder history now records the order of new saves independently of the system clock. After a clock change, the timeline and automatic history use the last saved version, not the version with the latest date. Existing versions remain readable without a migration.

## 0.38.0-41 (private alpha candidate)

- Disks also listens for Haiku's final partition mount and unmount notifications, so it can refresh after another application changes a volume.
- Mount and unmount actions recheck the filesystem label as well as the selected partition's location and geometry. A different label requires a fresh selection. Identical labels and geometry do not identify a physical disk uniquely.

## 0.38.0-40 (private alpha candidate)

- In Disks, right-click a recognized unmounted volume to mount it, or a mounted volume to unmount it. The boot volume cannot be unmounted from Listi. This does not change the known FAT disconnect limitation.

## 0.38.0-39 (private alpha candidate)

- Archive edits stop before publication if a private copy fails. Creating an archive claims its new filename exclusively; allow space for the staged archive and its final copy. A failed final write can leave an incomplete file and a recovery stage.
- Downloads and archive member copies use the final drive’s overwrite policy. Partial folder transfers keep their individual receipts.
- Metadata edits require a complete recovery record. Clipboard saves, converted images and CSV exports check the destination directory before reporting success.
- Reloading a folder preserves the selection and scroll position. Search results retain sorting by original folder.

Concurrent naming remains a limitation for ordinary copy, rename, same-volume move and publication of an extracted folder. Do not have multiple jobs or programs create the same destination name. The interrupted FAT write issue remains open.

## 0.38.0-38 (private alpha candidate)

- Correction for users of revision 37: do not follow the USB disconnect test in that package. A physical disconnect during copying was followed by a kernel panic. Finish the transfer and unmount before removal. This revision does not fix the FAT driver.
- Folder comparison and upload report inaccessible paths instead of treating an incomplete folder as complete.

## [0.38.0-37 alpha](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-37)

This public package includes the changes in revisions 24 through 37 listed below. Interrupted FAT writes can still crash Haiku; finish the copy and unmount before disconnecting the drive.

- Suspended folder tabs no longer reopen a retired location after Haiku reports its volume was unmounted.

## 0.38.0-36 (private test candidate)

- When Haiku reports that a volume was unmounted, file panels browsing it clear their selection and return to Disks. Panels on other volumes stay where they are.

## 0.38.0-35 (private test candidate)

- The CLI identifies its owning folder tab. Right-click a tab to move it across the window or detach it to Einn.
- Window → Move tab to window moves the active tab to a selected Listi window without closing other tabs or interrupting jobs in the source window.

## 0.38.0-34 (private test candidate)

- The separate Operations window is a normal Haiku window rather than a floating palette. Stack or tile it, or leave it behind the browser. It comes forward when a job needs a decision.
- Detach a tab to Einn from the folder context menu. Its pane’s CLI follows it without restarting.
- A detached CLI starts at the size of its file panel. Shelf opens narrower, at about half the area of the active panel.
- The Inspector uses the panel color and a thin border instead of a white box. Both docked and detached views have a compact button, no duplicate heading and a bottom-aligned empty prompt.
- Stop active directory scans when Listi detects that their volume was lost. This does not fix the Haiku kernel fault during interrupted FAT writes.

## 0.38.0-33 (private test candidate)

- Open Disks panels update automatically when Haiku mounts or unmounts a volume.
- On ext4 volumes without Haiku Trash, Listi offers permanent deletion only after explicit confirmation.
- Copying to ext4 can complete when the Haiku driver cannot write file attributes. Listi reports the omitted metadata.

## 0.38.0-32 (private test candidate)

- Choose whether file operation progress, errors and decisions appear in the browser or in a separate window. Existing settings keep the integrated view.

## 0.38.0-31 (private USB test candidate)

- After a removable disk reports an interrupted transfer, Listi blocks further access to the affected location across its windows. Switch to a local folder to continue. Safely reconnect and mount the disk before restarting Listi to use it again.
- This is an application-side mitigation for the physical disconnect reported on revision 30. It is not a Haiku kernel fix. Physical verification is still pending.

## 0.38.0-30 (local candidate)

- Detach the live Inspector, move or resize its window, and dock it again without losing the selection or an open text draft.
- Network and archive downloads retain local publication details and metadata warnings in the job report. A queued archive extraction no longer publishes a partial result when its private copy fails.
- Open a saved history store directly to restore a new copy even when the original folder is gone. This mode cannot save new versions.
- Manual and automatic folder history verify reused file contents before accepting a version. Large collections of BFS attributes are bounded during capture; restoring a symbolic link preserves its modification time.
- When detaching a folder tab during a CLI session or file operation, the new window identifies the original window as the owner of that work.
- Large tab collections no longer lay out hidden labels on every change. Cycling a single tab leaves its scan alone.
- Batched Shelf changes avoid repeated layout work, and archiving many local sources resolves the destination folder once.

## 0.38.0-24 (local candidate)

- Folder tabs sit together at the left edge of each file panel. Close a tab with its × without changing the panel width.
- When an open CLI prevents a window or the application from closing, cancel to keep working or explicitly close its CLI sessions and continue. Other quit blockers now explain what needs attention in a dialog.

## [0.38.0-23 alpha](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-23)

- Added local BFS folder versions and an optional low-priority Deskbar history helper. Browse saved points by date in a window or in the opposite panel. Restore a single regular file or an entire folder into a new path.
- Added named working sets with a preview, Copy to window, guided setup, compact Einn transitions and Mini icons. Restored searches stay out of Recent folders; unchanged results still report a stopped live search.
- Copy and Move show their destination. Conflict choices are per item unless explicitly applied to the batch. Trash restore shows original paths. For an ordinary copy to FAT, Listi can request consent to omit unsupported Haiku attributes without relaxing Move safeguards.
- Simplified successful job feedback, distinguished previous errors and selected rows, and added New file on the local folder background. Improved Inspector transitions, screenshot scaling, ZIP creation and the transparency explanation for JPEG conversion.
- Includes large-directory batching, FAT32/NTFS copy improvements, fractional file times where available and protection against silent replacement of network account credentials. See [verification and limits](docs/VERIFICATION.md).

## [0.38.0-13 test candidate](https://github.com/kambuziarendal/listi/releases/tag/test-4515)

Added independent windows and tabs, search as a file list, Keep both for local copy conflicts, and selection/list export. See the release record for that candidate.

## [0.38.0-12 alpha test candidate](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-12)

- An open destination folder now shows newly copied files in batches. Finishing or cancelling a job still refreshes the final listing.
- Recording recoverable file operations costs less during small-file copies. The journal is still written; this is not a fast mode that skips safety checks.
- The native qualification tools exercise interrupted jobs, storage pressure and the installed application. See [what was tested](docs/VERIFICATION.md#folder-refresh-measurements-0380-development). The revisions below describe source milestones, not separate public downloads.

## 0.38.0-6 alpha test candidate

- Network copies read the staged upload back before replacing the destination.
  A mismatch, unreadable stage, or cancelled verification leaves the previous
  target untouched. This requires read access and an additional download.
- Editing an existing archive checks every rebuilt member before replacing the
  archive. A failed content check leaves the original archive in place.

## 0.38.0-5 alpha test candidate

- Move between local volumes now keeps each completed original in its source
  volume’s Trash, rather than trusting a cached write enough to delete it.
  USB data streaming is supervised in both directions. Use Copy when the source
  volume does not support recoverable Trash. Cross-volume archive moves still
  require Copy.

## 0.38.0-4 alpha test candidate

- A stalled USB data write or flush no longer has to finish before the copy queue can respond to cancellation or disappearance of the destination device. The outstanding request only owns a private staging file; it cannot publish the destination or remove the source. A temporary file can remain after removal. Cross-disk Move is still disabled. A panel reading the interrupted destination lets you navigate away without waiting for the old reader.

## 0.38.0-1 alpha test candidate

- Choose Flat View in the panel’s view selector. Select a file to inspect it, double-click to open it, or select multiple originals to copy.
- File panels reclaim space from scrollbars when their contents fit.

## [0.37.1-2 alpha test candidate](https://github.com/kambuziarendal/listi/releases/tag/v0.37.1-2)

- Connect suggests the first readable standard SSH identity in your home directory. You can still enter a custom path. Connect prompts locally to unlock encrypted SSH keys. Text entry opens with a blinking insertion point.
- Panel errors wrap at the available width. A failed folder or SFTP address keeps its error visible; press Enter in the address field to retry it.
- Settings categories remain accessible in a narrow window, and short windows can scroll the current page without hiding Save or Cancel.

## 0.37.1-1 alpha test candidate

- Edit text inside Inspector and Quick view. A draft stays bound to its original file when the selection changes or the editor is detached.
- Open Flat View for the active panel in any workspace, resize the standalone Shelf, and jump to recent folders.
- Improve file-selection identity checks, same-volume moves, archive recovery, and clipboard handling so stale selections do not silently retarget operations.
- Keep folder differences visible in large comparisons and preserve photo orientation and filename extensions during image conversion.

This is still an alpha. The physical USB disconnect test has not been run. Use Copy for transfers between disks and check the destination after remounting before removing originals.

## [0.37.0-1 alpha test candidate](https://github.com/kambuziarendal/listi/releases/tag/v0.37.0-1)

- **Open with** can open a local file once or set Haiku's default application
  for its file type. A file's own preferred application still takes precedence.
- Each file panel has its own CLI session. Move a running session between the
  shared dock and its own window without restarting it. Show or hide function
  buttons from **View**, including in Smiðja.
- Choose a sidebar per panel with folder shortcuts, a tree, Shelf or **Recent
  folders**. Add shortcuts from the file list or tree; recent folders are kept
  per panel for the current session.
- Type the beginning of a name to select it in list, icon or Miller views.
  Context menus offer preview, folder views and selection commands, and search
  results can jump to the original file.
- Close Quick view with **×** or **Esc** to return focus to the file list.

This is an alpha, not a beta. The known wait after **Cancel** was observed
when a virtual USB disk was disconnected during a copy in 0.36.1-3.
The corresponding physical USB test has not been run.

## [0.36.1-3 test candidate](https://github.com/kambuziarendal/listi/releases/tag/v0.36.1-3)

- In Smiðja, switching a file panel to Search no longer leaves a hidden
  file available for editing in Inspector or Quick view.
- Copy and merged moves preserve attributes and timestamps on symbolic links,
  including links whose target no longer exists. Copy reports an incomplete
  folder if its entries change during enumeration.
- Restarting an archive edit keeps the previous draft recoverable until the
  replacement session is ready, including after an interrupted restart.

- Move refuses transfers between different local filesystem devices, including
  moves between a local archive and another volume. Use Copy and verify the
  destination before removing the original. Same-volume moves remain available,
  including valid Shelf items when a different selected item was replaced.
- A local disk copy no longer confirms a cached result when its backing disk
  has disappeared before publication.

## 0.36.0 (included in 0.36.1-3; not released separately)

- Smiðja can keep search results and previews in temporary panel tabs without navigating away from a file list.
- Open the layout editor directly from Settings, or move the running shell from its own header.

## [0.35.0 alpha](https://github.com/kambuziarendal/listi/releases/tag/v0.35.0)

Download `Listi-0.35.0-1-x86_64.hpkg` and its checksum from the release. See [what was tested](docs/VERIFICATION.md#0350-package-results).

- Quick view follows the active selection across workspaces. The inspector adds
  local photo controls, optional camera details and explicit short media previews.
- The source editor can edit bounded UTF-8 files up to 256 KiB, check for
  external changes and preserve original bytes for recovery before saving.
- Saved Shelf collections, typed BFS attribute columns and indexes, and a shell
  that can move between a panel and its own window expand the native workflows.
- Merging a moved folder retains a source folder if removing it would discard
  BFS metadata. Upload verification checks the staged file before publication.
- Select by pattern previews the number of matching visible names before
  replacing the selection. It does not change the panel filter.
- Local directory scans with icons deliver a small first batch so the panel
  can start drawing before the next regular batch is ready.
- Partial and interrupted jobs no longer display full progress as if they
  completed successfully.
- Find files restores its controls if the worker cannot deliver its result.
  A damaged recovery journal no longer hides other recoverable archive edits.
- Editing an encrypted archive keeps its protection even if the cached password
  has expired. Ordinary ZIP creation does not require AES support.

### Known limits

- Cancelling a job does not undo completed work. After an interruption, compare
  source and destination before retrying. A hidden staging file is not a backup.
  Local copies refuse empty typed BFS attributes rather than dropping them.
- Avoid renaming or replacing the same entries in another application during a
  move or rename. Haiku cannot atomically check identity and rename in one step.
- Settings writes preserve the previous file on reported write, file-sync,
  close or rename failures. This is not a guarantee against sudden power loss.
- Large remote folders may still take time to show their first entries.
  Archive support depends on installed engines. See the [workflow guide](docs/GUIDE.md)
  and [verification limits](docs/VERIFICATION.md#verification-limits).

## [0.32.1 alpha](https://github.com/kambuziarendal/listi/releases/tag/v0.32.1)

- Corrected device-icon scanning and retained shared ancestor columns when
  navigating in Einn.
- Download: `Listi-0.32.1-1-x86_64.hpkg`. See the release for the matching
  SHA-256 checksum.
