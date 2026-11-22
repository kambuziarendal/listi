# Changelog

## 0.38.0-2 alpha test candidate

- A stalled USB data write or flush no longer has to finish before the copy queue can respond to cancellation or disappearance of the destination device. The outstanding request only owns a private staging file; it cannot publish the destination or remove the source. A temporary file can remain after removal. Cross-disk Move is still disabled.

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
