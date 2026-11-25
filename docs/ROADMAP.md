# Roadmap

## The ambition

Listi aims to be a capable native working environment for files on Haiku: simple
enough for quick browsing, but deep enough for sustained, selection-oriented
file work. Directory Opus inspires the source/destination model and freedom of
action. Haiku's controls, attributes, queries and application ecosystem shape the
implementation.

The long-term ambition is broad coverage of the practical tasks people use
established file managers for, with a coherent Listi workflow rather than copied
interfaces. Coverage, responsiveness and resource use must be demonstrated task
by task. This is a direction, not a claim that the alpha already matches or
outperforms Directory Opus, Tracker or Dolphin.

The current [test candidate](../README.md#install) is available for Haiku x86_64. See the [feature tour](FEATURES.md) for what you can try today. The ideas below are not a release schedule.

## Current priorities

Everyday responsiveness and trustworthy file operations come before new workflows. Compare navigation and cancellation with Tracker on the same Haiku machine, measure small-file copy time separately, and preserve the journal and staged-write checks. [Earlier refresh measurements](VERIFICATION.md#folder-refresh-measurements-0380-development) are from a VM; there is no physical-hardware or Tracker benchmark yet.

1. **Before beta: interrupted file jobs.** In 0.36.1-3, late removal of a virtual USB disk
   left a copy waiting for over 100 seconds even after cancellation was requested.
   Revision 0.38.0-4 isolates writes and flushes to the private staging file.
   Disconnecting a physical USB disk during a copy remains untested. Keep
   this priority open pending the [physical USB test record](VERIFICATION.md#physical-usb-disconnect-during-copy).
   The window must stay responsive and unrelated jobs must be able to proceed. While a write can still return, report
   “Cancellation requested; waiting for device”, not “Cancelled”. Prevent a
   second write to the same target until the first is resolved. Check the
   original, previously copied files and the reported outcome after remount.
2. **Search and independent tabs have shipped in the 0.38.0-13 test package.** A pane can show a live BFS search as a navigable file list. Each pane supports independent folder tabs, including detach. Named working sets, their folder preview, compact Einn windows and the opt-in BFS history helper are included in 0.38.0-23; see the [versioned feature overview](FEATURES.md#browse-in-a-window-that-fits).
3. **Paired navigation remains a goal.** Link two folder roots by relative path only when requested. If a peer folder is missing, say so and leave it in place; never create or sync files while browsing.

**Keep the architecture lister-first.** Let folders and queries feed the same
panel without tying their state to a particular window layout. Extract shared
commands as real workflows need them, not in a wholesale rewrite. The first
custom button should be **Open selected in [app]**: choose the app once, then
pass the current selection on click without changing MIME defaults. A saved
bulk-rename template with a preview can follow.

## Optional by design

**Depth should be available, not compulsory.** A straightforward file browser
should remain a complete way to use Listi. Advanced views, inspectors, automation,
Git integration and other specialist tools should be optional and enabled when
useful—not imposed on every workspace.

The customisation goal goes beyond toggling a few controls:

- Add, remove, move and reorder action buttons, toolbars and workspace modules.
- Arrange panels, trees, inspectors, filters and an optional terminal around the
  task. Keep keyboard access and readable layouts at different window sizes.
- Save personal layouts and restore defaults without losing unrelated settings.
- Keep a useful standard layout for people who never customise anything.
- Make adaptive behaviour an explicit choice, with manual control and a
  predictable way back. Do not silently rearrange someone's workspace.

Version 0.32.0 has four presets and a workspace editor: choose and order command buttons, place modules in supported
zones and save named layouts. Optional adaptive sizing reflows the button bank;
it does not choose the active panel or view mode. This is not arbitrary docking
or a system for creating new commands. Safety checks, confirmations and
clear operation targets are not optional just because a tool is configurable.

## First: make existing file work easier to trust

Included in the 0.32.0 alpha:

1. **File safety and partial moves.** Local operations report what happened to
   selected entries. In a move that copies a tree before removing the source,
   individual entries distinguish committed moves from copies whose sources
   Listi did not remove. Native fault tests cover permissions, interrupted
   writes, full volumes and cancellation at different stages of a move.
2. **Job identity and results.** Jobs have IDs from acceptance. In Operations,
   Details lets you review a job after another one starts. Completed results
   do not depend on progress notifications. Cancel one job or cancel the queue;
   neither action undoes completed work.
3. **Shared command handling.** File actions share command IDs across menus,
   buttons and shortcuts. Transfer entry points reuse the same preparation
   and guarded worker operations rather than implementing their own copies.
   Clipboard ownership, archive verification and drop choices retain their
   specific checks.

4. **Selection and retries.** Refreshing a local folder no longer selects a
   replacement file merely because its name matches. Colliding paste/drop names
   are rejected before work starts. If the original parent now points into
   Trash, restore keeps the entry and its metadata so you can retry.

See the [workflow guide](GUIDE.md) for interpreting partial results and the
[verification guide](VERIFICATION.md) for reproducing fault tests. Job reports
are session information, not a persistent audit log or an undo mechanism.

## Existing foundations and later ideas

### Implemented in the 0.35.0 alpha

- **Wildcard selection:** preview the number of visible matches before replacing the selection, without changing the filter.
- **Direct Paste:** ordinary Paste in a local file panel now accepts file
  references, text and images. Text and images open a filename prompt; Paste
  in an editable field retains its normal meaning.
- **Flat View:** files from a local folder tree appear in one recursive list
  with relative parent folders. Inspect, open or copy selected originals, or collect them on the Shelf. Multi-folder rename is still a future extension.
- **Bulk rename:** dependency-ordered chains, swaps, exact row overrides and
  guarded undo of the last eligible batch in this session. Regex, date/time
  and typed BFS placeholders remain possible extensions.
- **BFS attributes:** Properties can add typed Details columns and create or
  verify a volume index with explicit confirmation. A query checks the
  selected entry; old values are not silently rewritten.
- **Named Shelf collections:** save and restore references, follow verified
  BFS identities within bounded searches, or explicitly relink a missing or
  ambiguous member. A collection is not a backup or a saved file job.
- **Workspace and CLI controls:** choose trees or favourite destinations per
  Smiðja panel; toggle the Tvískipt button bank. Keep a live shell below or
  beside the panels, detach it into a window, or dock the same session again.
- **Quick view and the inspector:** toggle a preview beside the selection in
  any workspace or open a separate viewer. Zoom and pan images, read JPEG EXIF
  locally, reveal GPS coordinates by choice, and open local media in MediaPlayer.
- **Native source editing:** explicitly edit complete, writable local UTF-8
  text, with syntax colouring, find/replace, go to line and guarded saves.
  This is not an archive editor, binary editor or document renderer.

See the [workflow guide](GUIDE.md) for the controls and limitations.

### Other follow-ups, not prerequisites

- **Jump to a visited folder.** From Go or a keyboard shortcut, type parts of
  a path (for example, `lis src`). Rank visited folders by frequency and recency
  and show their full paths. Enter opens the chosen folder in the active panel;
  Esc leaves the panel and its history unchanged. This recalls places the user
  has visited; it does not scan the disk or replace live BFS search.
- **Checkbox selection.** While reviewing files, keep checked items separate
  from the focused row so browsing and Quick view do not clear the set.
  Start with an explicit **Add N checked items to Shelf** action. Ordinary
  Copy and Move must not silently switch from selected rows to checked items.

- Add an on-demand, cancellable folder-size scan. Distinguish logical bytes
  from allocated space, define hard-link counting and flag unreadable subtrees.
  Do not slow down ordinary selection summaries.
- Investigate Tracker add-on discovery, MIME matching and invocation from a
  non-Tracker host on Haiku, using a frozen selection. Prefer proven native
  compatibility to a new plugin API; defer execution until that contract works.
- Consider per-transfer network rate limits after measuring contention.
  After rename undo is proven, evaluate undo for creating a still-empty folder
  or restoring an unchanged Trash entry; do not promise general undo.

### Engineering work that supports this sequence

- **Keep the native test runner as the reference.** Add focused suite selection
  and a failing exit status for missing or failed required checks where needed.
  Keep the [portable CI checks](CI.md) distinct
  from native Haiku builds, fault tests and installed-package checks.
  Do not make paid or hosted Haiku capacity a prerequisite.
- **Split App.cpp at a real change boundary.** At 2,314 lines in 0.32.0, extract
  the command or panel responsibility touched by the first workflow, with tests
  for existing menu, button and keyboard behaviour. Avoid a wholesale rewrite.
- **Localise incrementally.** Introduce Haiku translation contexts in the first
  changed workflow, including errors and plural counts. Verify catalogue
  extraction, fallback text and layout with a longer translation before rollout.
- **Make releases easier to follow and report against.** Maintain the [changelog](../CHANGELOG.md) with user-visible changes and compatibility notes; add SECURITY.md
  once a monitored private reporting route is confirmed. Keep reproduction
  instructions in the existing verification guide.
## After beta1: candidates for prioritisation

These seven ideas are for discussion after beta1 ships. They do not change the
beta1 plan or its order. Until then they are documentation only: no
implementation, prototypes, tests or preparatory refactoring. Consider the copy
plan and BFS batch actions first; none of the seven is a release commitment.

1. **Turn folder comparison into a copy plan.** Choose the copy direction, see
   new, changed and identical files, inspect conflicts and select individual
   entries. Show estimated space for the selection before sending it to the job
   queue. The first version would recheck changed sources and copy only approved
   entries. A comparison is not permission to write. Consider deletion or
   mirroring separately: absence alone does not identify a deletion. Two-way
   or remote synchronisation would need a separate review.
2. **Apply BFS attributes to search results.** Preview the existing and proposed
   typed values on selected hits across folders before applying a batch edit.
   For example, tag selected photos with a project attribute, then open a saved
   live query showing all photos with that tag.
3. **Resume a Working Set.** Extend today's saved windows, tabs and layouts to
   restore open tabs, the active panel, sort order, selected file and the
   Smiðja terminal's working directory. Open the available parts even if a path
   is missing: identify a missing folder and offer to locate it, leave a missing
   file unselected, and show any fallback terminal directory. Do not recreate
   missing files or replay shell commands or file operations.
4. **Follow up on past operations.** Keep completed and interrupted jobs available
   after restart. Distinguish what finished from what failed, and show any
   recovery paths. Let the user prepare a new job from failed entries, with
   source and destination visible for review before running it. A journal is
   neither a backup nor an automatic retry or undo.
5. **Compare folder history with the present.** Compare a saved folder version
   with the current folder. In the Inspector, show a side-by-side text diff or
   old and current image previews. Restore only selected files as new copies,
   without overwriting current files. Comparison need not imply editing or
   merging, and unsupported formats should be identified.
6. **Find an action from the keyboard.** One shortcut opens a palette for
   actions, folders and Working Sets. Use the active panel's context to offer
   relevant choices.
7. **Review duplicates in Flat View.** Search explicitly chosen locations,
   group candidates by size and confirm identical contents with SHA-256. Show
   each location and any differences in BFS attributes before the user chooses
   which copies to keep. Review the existing prototype rather than importing it
   wholesale. Before integration, require native coverage for hard links,
   unreadable or changing files and cancellation, followed by read-only group
   review. Finding a match must never delete a file automatically.
   The existing Duplicate command still means “make a copy.”

## Feature directions and current foundations

In this historical comparison, “Available now” means included in the 0.35.0 alpha. A related foundation is not a finished feature.

| Direction | Available now | Planned or exploratory work |
|---|---|---|
| Duplicate finder | SHA-256 folder comparison; Duplicate creates another copy | Review verified duplicate groups in Flat View after beta1 |
| Bulk rename | Local preview, exact row overrides, swaps, collision checks and guarded undo | Broader rename transformations |
| Miller columns and adaptive views | Four presets, optional Miller columns and opt-in compact buttons | Automatic file-view changes remain exploratory |
| Directory synchronisation | Read-only recursive size/SHA-256 comparison | Start with a reviewed one-way copy plan after beta1 |
| Clipboard contents to a new file | Panel Paste accepts file references; pasting text or an image creates a new file without replacement | Explore additional clipboard formats when useful |
| Rich built-in inspectors | Quick view, standalone viewer, native text editor, zoom/pan, offline JPEG EXIF and optional GPS display | Broader metadata and richer support for other formats |
| Copy queues and pause | Per-job results, pause/resume and targeted cancellation | A persistent operations journal is an after-beta1 candidate |
| Automatic rules and drop folders | Manual file actions, search filters and live BFS queries | Opt-in sorting rules and configured drop targets for extract/rename/move workflows |
| Colour coding and tags | Configurable file/folder/link colours and typed BFS attribute editing | Dedicated per-item labels, tag editing/filtering and optional colour rules |
| Saved project sessions | Named layout profiles, four presets, saved places and folder view modes | Extend Working Sets with session restoration after beta1 |
| Git status in file lists | No built-in Git status display | Optional status badges or columns without blocking browsing |
| Built-in text/file diff | Read-only folder comparison by size or SHA-256 | Compare a saved version with current files after beta1 |

## More depth without more friction

### Work with selections

Listi previews local bulk renames with search/replace,
numbering, prefix/suffix, collision checks and guarded execution. Configurable
actions should operate on frozen selections and pass filenames as data, not
shell code.

### Follow deep folder trees without losing context

Miller columns are a file view, independent of the workspace. Each workspace starts
in Details and has a configurable default view. Explore adaptive views only
with explicit user control and sensible minimum sizes.

Named working sets in 0.38.0-23 already reopen selected windows, tabs and layouts without replaying operations. Readable, BeOS-inspired folder cards and linked navigation between paired roots remain goals.

### Inspect files without leaving the task

Quick view, the separate viewer and the panel inspector share file readers.
They show images, readable source, PDF pages and supported audio/video; viewing
does not execute scripts or change the original file. The 0.35.0 alpha adds zoom/pan,
JPEG camera details and a separate, offline GPS display. Text editing is an
explicit action on complete local files with guarded saves. Future format
support should preserve those boundaries and distinguish unsupported metadata
from a missing value. See [Inspect and edit a file](GUIDE.md#inspect-and-edit-a-file).

### Turn repeated chores into opt-in workflows

Explore rules based on names, extensions/types and dates, with named target
folders and explicit extract, rename or move actions. Configured **drop folders**
could apply a chosen workflow when files are dropped there. These are not the
same as read-only live BFS query results.

Start with visible, explicitly configured triggers and previews/dry runs. Avoid
hidden background moves, recursion loops or overwriting files without review.
A watched folder, a drop target and a general scripting hook have different
trust and lifecycle requirements; none is implicitly enabled by this roadmap.

Listi can save plain clipboard text or an image as a new named
file in a local folder. Images are saved as PNG. File references still use
ordinary Paste.

### Collect files from several folders on a Shelf

Listi has a separate Shelf window for local
references, with original-folder and availability columns, drop support,
Command+Shift+Y, and queued copy and move actions.
Create archive packs references from different folders in a chosen format
without moving the originals. Named snapshots preserve references across
sessions; the working Shelf itself remains session-only. Sharing is still
planned. Verify Stack & Tile ergonomics during hands-on testing.

### Send a selection to a phone

Plan a **Send to mobile** context-menu action. On request, start a temporary
local web server for the selected files and show a QR code. A phone on the same
network scans the code to download them in its browser, without a cloud account.
Serve only the selection through an unguessable, short-lived link, not its
parent folders. Show when sharing is active, when it expires and how to stop it.

### Use Haiku's desktop tools

- Let tool windows such as the Shelf participate in native **Stack & Tile** or
  remain independent, rather than implementing Listi's own docking system.
- Listi offers **Convert here** for a single local image drop,
  using installed Translation Kit output formats without overwriting files.
  Ordinary Copy and Move choices remain available for cross-folder drops.
- Explore an optional desktop **Replicant** as a drop zone. A first use could be
  adding files to the Shelf without bringing a Listi window to the front.

### Organise and inspect project context

Explore optional file labels/tags, colour coding and filtering using native
metadata where practical. Category colours and a generic attribute editor are
useful foundations, not a finished tagging system. Explain persistence and what
happens when files move to a provider that cannot preserve the metadata; colour
must not be the only signal.

Optional **Git status** could show modified, untracked, staged and ignored state
in the file list. Keep discovery and refresh bounded and asynchronous, with a
clear unavailable state outside repositories or when support is missing. Merely
browsing must not change repository state or fetch from a remote.

### Broader archives, search and network workflows

Make archives feel like folders wherever each format permits, while showing
capability and metadata limits explicitly. Selected encrypted and multipart
reading, local editing, and archive/local moves already exist; remaining work
includes broader variants, metadata fidelity and supported write engines.

Build on saved searches, date/size/extension filters and live BFS queries with
clearer scopes, exclusions, attribute tools, content search and broader volume
coverage. Results must retain a clear connection to originals.

Continue testing interrupted transfers across protocols, servers and larger
datasets. Availability of SMB, SFTP, FTP or FTPS does not qualify every server or
WAN condition.

## Longer-term possibilities

- **Conflict-aware recovery:** consider limited undo separately from the proposed
  operations journal. Recovery must not overwrite newer data.
- **Automation interfaces:** consider a local CLI or socket with explicit access
  boundaries. Scripting, hooks and plugins are not implied by an internal
  command registry or an optional rule editor.
- **Portability:** extract tested core logic incrementally, keeping Haiku UI,
  BFS, clipboard, Trash and application integration behind adapters. Other
  platforms need native filesystem, package and UI verification. A port is not a
  reason to replace the Haiku experience.
- **Desktop integration:** coexist with Tracker by default. A replacement role
  is optional, not a 1.0 requirement. It needs a separate compatibility study
  and a tested route back to Tracker.
- **Optional peer sharing:** a future adapter could connect to a separately
  developed sharing system; it is not a current dependency or shipped feature.

## How progress should be judged

Deliver small, complete workflows, measure them on documented environments, and
publish known limits alongside capabilities. A feature name, successful build or
large test count is not enough to establish a dependable user experience.

The aim is useful depth with a native feel, not a race to the longest checklist.
There are no fixed delivery dates, promised performance ratios or claims of
universal crash consistency.
