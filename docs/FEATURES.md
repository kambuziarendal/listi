# What you can do with Listi

Listi is a native Haiku file manager. Browse one folder at a time or set up a
source and destination side by side, then use the same menus, clipboard and
file actions in either layout.

This tour covers the [0.37.0-1 alpha test candidate for Haiku x86_64][release].
Follow the [installation instructions](../README.md#install), then try the examples with
disposable files. Installing an older package does not reverse file operations.

For a first session, [copy a selection between panels](#select-copy-and-move-files),
[compare two folders](#compare-folders-without-changing-them), then
[inspect a file without leaving the list](#inspect-or-edit-what-you-are-looking-at).

## Set up the window for the task

Choose a layout under **View > Workspace**:

| Workspace | Try it when you want to… |
| --- | --- |
| **Einn** | Browse in one panel, with no second location competing for space. |
| **Askr** | See the folder hierarchy and the active file list together. |
| **Tvískipt** | Keep a source and destination visible for transfers. Use **View > Show function buttons** to hide or show the function buttons. |
| **Smiðja** | Keep two panels alongside tools such as the inspector, with independent sidebars for shortcuts, tree, Shelf or recent folders. |

Switching workspaces does not clear the panels or restart queued jobs. In a
two-panel layout, use **Tab** to switch the active panel. Each side has its own
location and Back/Forward history. Enter a path directly, use **Go** for common
locations, or save a named location in **Places**. **Listi > Same folder in other
panel** gives both sides the same starting point; moving through one does not
automatically navigate the other.

Under **View > File view**, choose **Details**, icon views, **Thumbnails** or
**Miller columns**. Miller columns expose the ancestor folders as you navigate.
For a folder you always want to see in a particular view, use **Remember view
for this folder**. That remembers the display mode, not a whole workspace.
Sort by **Name**, **Size** or **Modified**; selecting the same file type is a
different action, not a sort order.

Open **View > Workspace > Customize workspace…** to reorder existing command
buttons, position workspace modules in supported zones or stack the panels.
Profiles save layouts, not open folders, selected files or jobs. The optional
compact button layout helps on narrow windows. Menu commands and shortcuts
remain available even if their buttons are hidden.

![Two file panels in Haiku's dark appearance, with a file context menu open](screenshots/listi-tviskipt-context-dark-0.31.0.png)

*Haiku capture from 0.31.0, when Tvískipt was labelled Commander.*

## Select, copy and move files

1. Open a disposable source folder and a separate destination folder in
   **Tvískipt**. Click the source panel, then select a few files.
2. Choose **Files > Copy to other panel** (Command+Shift+C). The active panel
   is the source; the other panel is the destination. For **Copy** or **Cut**
   followed by **Paste**, the destination is whichever folder is active when
   you paste. This works in a single-panel layout as well.
3. Open **Listi > Operations**, then **Details**, to review the job. An accepted
   job retains its original selection and destination as you continue browsing.
   For a conflict, read the paths before choosing Replace, Skip or Abort.

Right-click the list and choose **Selected files** or **This folder** to change
what the menu acts on. **This folder** is the current folder, not a child
folder under the pointer. Dragging local files into a local panel prompts
for Copy, Move or Cancel; archive and network drops are copy-only.

For a shorter file list, use **Listi > Filter names**. This is a literal
name filter: hidden rows are deselected. For a wildcard selection, use
**Select > By pattern…** and enter, for example, `*.png`. Review the match
count before replacing the selection. Patterns are case-sensitive and match
only visible names; they do not change the filter or search recursively.
**Select > Same file type** instead matches the selected file's extension
among visible ordinary files, ignoring case and recognising compound TAR
suffixes such as `.tar.gz`. It does not test MIME types or file contents.

- **Paste as a new file:** Copy text or an image, focus a writable local
  file panel and use **Paste**. Enter a name or accept `Clipboard.txt` or
  `Clipboard.png`. Existing files are never replaced by this prompt. Paste
  in a text field still inserts text into the field.
- **Convert an image:** Drop one local image into a local panel, choose
  **Convert here**, then choose among formats offered by the installed Haiku
  translators. Listi creates a new file without replacing the source or an
  existing output.
- **Use Trash for ordinary deletion:** **Move to Trash** uses Haiku's Trash,
  with Tracker-compatible restore metadata for supported local entries.
  **Restore from Trash** and **Delete permanently…** are separate commands.
  An archive member removed from a container does not go to local Trash.

### Read the result before retrying a job

Operations can pause or resume a job, cancel the running job, remove a selected
waiting job or cancel the queue. None of those actions undoes committed work.
A local result marked **Copied; source retained** is not a completed move.
For a partially completed folder, check its individual entries in the report.
Remote and archive jobs may report only a job-level error or recovery path.
Job reports last for the current session. After an interruption, compare source
and destination before retrying; a hidden staging file is not a backup.
See [Panels and jobs][jobs] for result labels and recovery details.

## Find, collect and rename

- **One recursive file list:** Choose **Flat View** alongside Details and Icons to show
  the active panel’s local folder tree in that panel. The other panel stays
  available. The **Relative folder** column distinguishes files with the
  same name; `.` marks the starting folder. Filter by file name or relative
  folder, or stop the scan and keep its partial results. Flat View is a
  snapshot. Select a result to inspect its original. **Add to Shelf** collects references to the selected
  originals after checking they have not been replaced.
- **Find files by criteria:** Open **Listi > Find files…**. Combine a name
  fragment with extension, size or date filters. Use **BFS formula** for a
  native volume query, with optional **Live** updates, or **Save** to keep
  criteria for later. **Show original in panel** takes you to the original
  file. Search is on one persistent local volume, not across remote folders,
  archive contents or file contents.
- **Collect originals from several folders:** Use **Files > Add to Shelf** or
  drop local entries onto **Listi > Show Shelf**. **Copy all…**, **Move all…**
  and **Create archive…** act on the entire Shelf, not just highlighted rows.
  **Collections > Save current Shelf…** saves named references, not copies.
  BFS identity checks can recover some moves or renames; unavailable or
  ambiguous entries may need **Relink…**. Removing references does not delete
  files, and loading a collection does not run a job.
- **Rename a batch:** Select at least two entries in one local folder and
  open **Files > Bulk rename…**. Try a prefix, suffix, literal replacement
  or numbering, then **Update preview**. The rules preserve extensions.
  Use **Set name** to override a row with an exact name, including for a
  swap. Listi rejects duplicate outputs and names occupied by unrelated
  entries. With the queue idle, **Files > Undo last bulk rename** can undo
  the last eligible batch in the session if the entries still pass verification.

![Find files with saved criteria and extension, size and date filters](screenshots/listi-search-filters-0.30.0.png)

*Find files, captured in 0.30.0 with example files.*

## Compare folders without changing them

Open **Listi > Compare folders…** and choose the two locations. Start with
**Compare names and sizes** to spot missing entries and type or size differences.
Choose **Compare with SHA-256** to detect different contents in equal-size
files. **Show matching entries** includes matches in the results; select a row
to inspect its paths, digests or errors. The comparison does not copy, merge
or synchronise anything. It also does not compare permissions or BFS attributes.
Keep the folders idle during comparisons, and expect remote hashing to read
entire files over the network.

![Folder comparison showing different SHA-256 digests for equal-size files](screenshots/listi-folder-compare-0.28.0.png)

*Folder comparison, captured in 0.28.0.*

## Inspect or edit what you are looking at

Select a file and toggle **Listi > Quick view** (Command+Shift+P). The view
follows the selection and temporarily hides the other panel in a two-panel
layout. Toggle it off to return to the layout you were using. **Files > View**
(Command+Shift+V) opens a separate viewer, while **Listi > Inspector panel**
can keep a preview alongside two panels.

- **Images:** scroll to zoom, drag to pan, or use **Image…** to fit the image
  or inspect it at 100%. **Camera details…** reads recognised JPEG EXIF fields
  locally. If there are GPS tags, revealing coordinates requires a separate
  choice; it does not contact a map service.
- **PDF and media:** use **Previous** and **Next** for PDF pages. **Play
  preview** samples supported audio or video for up to 15 seconds, depending on
  installed codecs. Use **MediaPlayer** for full playback of a local file.
  Selecting a file does not start playback.
- **Text and source:** **Edit** inside the preview opens Listi's native editor
  for complete, writable local UTF-8 files up to 256 KiB. **Try as text…**
  validates an unrecognised file before editing. Use Find and replace, Go to
  line, undo/redo and explicit Save. The editor preserves the original
  UTF-8 marker, line-ending convention and BFS attributes. If the underlying
  file changes, Listi refuses the save and keeps the edit open. **Files > Edit**
  is a different command: it uses the configured external editor.

![Quick view displaying a sunset photograph beside the active file list](screenshots/listi-quick-view-0.35.0.png)
![Native source editor with syntax colours and editing controls](screenshots/listi-source-editor-0.35.0.png)

*Haiku captures from the 0.35.0 source build.
[Capture details and photograph credit](screenshots/CREDITS.md).*

## Use Haiku attributes, applications and the shell

Open **Files > Properties…** on a local entry to inspect timestamps,
ownership, permissions and typed BFS attributes. Supported permissions and
attributes can be edited explicitly; system namespaces and unsupported binary
data stay read-only. Select an attribute and use **Show in Details** to add its
column to the originating panel. Use **Make searchable…** to check or create a
supported BFS index on the volume. Creating an index does not backfill old
values. For native lookup use Find files' BFS query mode.

For one local regular file, **Files > Open with… > Open once** picks an
installed application without changing the system's associations. **Listi >
Terminal here** opens an interactive shell in a local directory. **Left CLI**
and **Right CLI** start separate shells from their respective file panels and
can stay open together. **Dock position** moves the shared dock below, right,
above or left of the file panels. Either CLI can detach without losing its
running commands or scrollback. **External terminal window** launches the
configured separate terminal instead.

## Work with archives and network folders

**Archives.** Open a supported archive to browse its members in a panel. Use
**Files > Archives** to create or extract an archive. Formats, compression and
password options depend on installed engines. Reading, creating and editing
are separate capabilities: for example, selected single-volume RAR5 archives
can be rebuilt in Store mode, but RAR4 writing, split and nested mutation are
not supported. A supported writable local archive can accept clipboard
additions, new folder creation, renames and removals through a rebuild.
See the [archive format and recovery guide][archives] for exact limits.

For a regular member in a supported writable archive, **Files > Edit** opens
a working copy in the configured editor. Listi can write stable saves back
through a verified rebuild. **Files > Archives > Resume archive edits…**
reopens retained edit sessions. Working copies, including decrypted ones,
can remain on disk. Archive moves that verify a destination before removing
a source are still not an atomic operation across both locations.

**Network.** Open **Listi > Connect to server…**, enter an SMB, SFTP, FTP or
FTPS address, and supply credentials in the separate fields. **Advanced**
offers an SMB domain and SSH key/known-hosts settings. Credentials stay in
memory for the session. Plain FTP is unencrypted. Keep the connection in
one panel and a local folder in the other to transfer through the ordinary
job interface. Available operations and recovery guarantees depend on the
protocol and server; check both sides after a failure before retrying.

## Where to go next

See the [workflow guide](GUIDE.md) for controls and recovery, the
[changelog](../CHANGELOG.md) for version differences, and the
[roadmap](ROADMAP.md) for ideas not yet implemented. Folder comparison does not
synchronise, **Duplicate** makes a copy rather than finding duplicates, and
layout profiles are not saved project sessions.

*Language-model assistance was used during development and documentation.*

[release]: https://github.com/kambuziarendal/listi/releases/tag/v0.37.0-1
[jobs]: GUIDE.md#panels-and-jobs
[archives]: GUIDE.md#archives
