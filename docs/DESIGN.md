# Design and architecture

Listi is a native Haiku file manager inspired by Directory Opus-style
source/destination file work. It is an independent implementation, not a port.
Its interface uses Haiku controls, system colours, MIME icons and Layout Kit.

## Design principles

- **Visible intent.** The active panel is the source; direct Copy and Move use
  the other panel. Clipboard Paste uses the active panel. Accepted jobs retain
  their selected paths and target when focus, navigation or workspace changes.
- **Shared actions.** Menus, buttons and shortcuts should use the same validation
  and file-operation logic.
- **Native keyboard behaviour.** Use Haiku's Command modifier and preserve text
  editing and list type-ahead. Function keys are unassigned in the current alpha.
  See the [shortcut reference](../README.md#basic-workflow).
- **Optional by design.** Useful defaults come first. Advanced views, inspectors,
  rules and integrations should be opt-in rather than required for ordinary
  browsing. The 0.35.0 alpha uses fixed module positions and an
  ordered bank of existing actions. Profiles contain layout state, not paths,
  credentials, selections, running jobs or pending confirmations. Configurability must not
  disable safety checks or hide operation targets.
- **Folder versions, not snapshots.** Local BFS history reuses objects for unchanged file bytes. Browsing a saved manifest is read-only. The optional Deskbar helper runs bounded checks outside the file manager; restoring publishes a new copy rather than rolling back a live directory. See the [history guide](GUIDE.md#manual-folder-history).
- **Honest outcomes.** Report cancellation, errors and partial completion.
  Cancellation is not rollback, and a job log is not a backup.

## Native components

- Panels use asynchronous directory scanning and node monitoring. Scan
  generations distinguish current results from obsolete replies.
- `OperationQueue` runs file work on a worker thread and communicates with the
  native progress interface. The filesystem engine does not manipulate widgets.
- Regular copies use temporary sibling files. Symlinks are copied as links rather
  than followed recursively. Moves check source identities before cleanup;
  concurrent external changes and partial moves remain important limitations.
- File reads, launches and settings writes use the service looper in `src/ApplicationServices.cpp`. Layout Kit
  arranges controls rather than relying on fixed widget coordinates.
- Haiku's private ColumnListView dependency is isolated in `src/Pane.cpp`.
  Changes to that OS interface may require a rebuild.
- Archive and network adapters share the panel and queue workflow. Format and
  protocol capabilities depend on the backend and must be reported explicitly.

## Preview and editing boundaries

Quick view changes the split layout, not the identity of either panel. The
active panel keeps navigation and selection; the hidden panel is not replaced
by a new file browser. Quick view, the panel inspector and the separate viewer
share readers. Selection changes invalidate stale results from the preview
worker; selecting media does not trigger playback or launch an application.

Text previews may read a bounded prefix. The native editor never uses that
prefix as a document: `Inspector::Document` opens the regular local file,
checks its complete bytes and remembers the original encoding marker and line
endings. The editor window owns its edits independently of the panel selection.
Command+E in the file manager still targets the configured external editor.

On save, the document locks its open node and compares the current path,
identity and bytes. It syncs an original-byte recovery file in the same folder
before updating the pinned node. Preserving the node also preserves its BFS
attributes. This is not an atomic replacement or a volume-wide lock:
descriptors opened by another process before the lock may still write.
Unverified restoration leaves the recovery path for inspection.

Image navigation transforms the view, not the image. The JPEG reader examines
bounded metadata segments; camera fields are separate from coordinates. GPS
display is opt-in and neither it nor camera inspection consults a map service.
Previewing a file on a network share can still transfer its contents.

## Trash and recovery

`src/Trash.cpp` uses native Haiku filesystem APIs and Tracker's
`_trk/original_path` attribute convention. Trash is on the source volume.
Restore preserves conflicts and requires the original parent folder to exist.
Unsupported volumes fail rather than falling back to permanent deletion.
Permanent deletion is a separate, confirmed action.

Listi is not a transactional backup system. See the
[workflow guide](GUIDE.md) for operation limits and the
[testing guide](VERIFICATION.md) for verification boundaries.

## Names and compatibility

The application, executable and launcher are **Listi**; the repository and
package ID are `listi`.

| Workspace | ASCII ID | Stored numeric value |
|---|---|---|
| Einn | `einn` | 1 |
| Askr | `askr` | 2 |
| Tvískipt | `tviskipt` | 0 |
| Smiðja | `smidja` | 3 |

The native `Workspace` scripting property returns the display name. Scripts
comparing that value must account for the 0.31.1 rename.

Existing numeric workspace values and legacy settings aliases are retained.
Unknown workspace values fall back to Tvískipt. Compatibility-only identifiers
still use the historical `Lister` spelling, including the application signature
`application/x-vnd.Kambuzi-Lister`, scripting suite, settings stores and package
aliases. No manual settings migration is required by the rename.

## Future architecture

Listi currently targets Haiku. Portable core extraction and additional platform
adapters are future work, not shipped components. Tracker coexistence remains
the default; replacing the desktop is outside the current scope. See the
[roadmap](ROADMAP.md).
