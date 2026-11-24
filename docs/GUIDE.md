# Workflow guide

This guide describes the development source. The [0.38.0-12 packaged alpha](../README.md#install) includes the latest transfer and recovery changes. New to Listi? Start with the [feature tour](FEATURES.md). Try file operations on disposable copies and keep independent backups.

## Window names and layouts

The file window title follows the active folder. Use **Window > Name window…**
for a role such as “Invoices”; the name stays put while you navigate. **Use
folder name** restores the automatic title. Neither choice changes the source
or destination of a file operation.

**Window > Open windows…** lists the file manager, detached Listi CLIs, editors
and standalone previews. Select a row to see its full location, rename it or
activate it. Equal names are allowed; the number identifies that open window.
A CLI shows where its shell started, not the shell’s current directory. Docking
a CLI removes its floating window from the list without stopping the shell.
Names last for the life of the window; they do not save a working set.

On a fresh installation, the optional layout introduction describes **Einn**,
**Askr**, **Tvískipt** and **Smiðja**. Selecting an option previews its
description; only **Use this layout** switches the file window. **Skip** or
**Cancel** keeps the current layout. Reopen it through **Help > Layout
introduction…**. Existing preferences are not reset on upgrade.

## Small features at a glance

Available actions depend on whether the location is local, in an archive or on
a network server. Unsupported actions are disabled.

### Right-click: the selection or the current folder

- **Selected files / This folder:** change the menu's target without leaving it.
  **This folder** is the panel's current directory, not a selected child folder.
- Right-clicking an unselected entry selects that entry. Right-clicking an
  already selected entry retains the multi-selection.
- **Selected files:** Open, applicable Open with/View/Edit, Copy/Cut, direct
  Copy/Move to the other panel, Rename, Duplicate, symbolic links, Trash or
  archive removal, archive actions and Properties as supported by the target.
- **This folder:** Sort by, View, New folder, Paste here, Refresh, Terminal here
  and Properties where applicable.
- **Select same file type:** replace the selection with visible ordinary files
  sharing the clicked file's case-insensitive extension. Folders and links are
  excluded; it is not MIME detection. Recognised compressed-TAR suffixes remain
  distinct from a plain compressed stream.

**Sorting and type selection are different:** sorting supports **Name, Size and
Modified**, each ascending or descending. **Sort by file type is not implemented**
in this alpha.

### Navigation, views and selection

- Independent Back/Forward history per panel; double-click folders, use an
  editable path, or go up with Backspace while browsing the list.
- **Go:** Parent, Root, Home, Desktop, Disks and Trash on the current volume.
- **Places:** save the current location under a name, rename it, forget it or
  reopen it. Saving a place is not saving credentials.
- **Same folder in other panel:** open the active location on the other side.
- **View:** Details, Icons, Large icons, Thumbnails and Miller columns; optional folder tree per
  panel. Details columns can also be used to choose sort order.
- **Remember view for this folder / Use pane view for this folder:** save or clear
  a folder-specific display-mode override. This is not a saved workspace or a
  promise to remember every column, tree or sort setting per folder. Right-click empty space and open **View** to change modes (including Miller columns), remember a folder view, or use the workspace default.
- **Type to select:** with the file list focused, type the beginning of a file or folder name to select it without opening it. After a one-second pause, the next letter starts a new match. This also works in icon views and Miller columns.
- **All, None, Invert:** apply to the visible list; filtered-out entries do not
  silently rejoin the selection when revealed.
- **Selection summary:** selected file bytes, folder/link counts and unavailable
  sizes are distinguished. Folder contents and link targets are not recursively
  included in that byte total.
- **Filter:** Clear or Escape removes the filter; Enter returns to the list.
  The filter is a literal name match, not a wildcard expression.
- **Keyboard selection:** Space toggles the focused entry; icon views also
  support arrows, Shift+arrows for range selection, Home/End and Enter to open.
  Tab switches panels in the dual-panel workspaces.

### Search and comparison conveniences

- **Save / Forget search:** keep named criteria rather than frozen result rows.
- **Show original in panel:** navigate from a search hit back to its location.
- **Use creation date / Include Trash / Live:** explicit search choices where
  applicable. Clear filters removes the extension/size/date conditions without
  clearing the name, BFS formula or search root.
- **Show matching entries:** include matches in folder-comparison results rather
  than displaying differences alone. Select a result for paths, digests and
  error details. Size-only equality is not content verification.

### Tools and preferences

- **View > Show function buttons:** toggle the button bank in Tvískipt or
  Smiðja. Each remembers its own choice. If the buttons do not fit, use
  **View > Function actions** instead.
- **Terminal here:** **Active lister** (Command+T) starts a shell in the active
  local folder. Choose **Left CLI** or **Right CLI** to open or focus the shell
  belonging to that file panel, even if the other panel is active. Both can run
  together. Navigating a file panel does not change its shell's working
  directory. Use **Terminal here** again from a different local folder to
  replace its docked shell after confirmation.
- **Dock position:** move the shared CLI dock below, right, above or left of the
  file panels. The **Move** control on either docked CLI cycles these positions.
  **Detach CLI** moves only that shell to its own window. **Dock CLI** or the
  detached window's close button returns it to the dock. Moving a CLI preserves
  running commands, shell state and scrollback. **Close CLI** ends only that
  shell.
- **Detached Listi CLI** (Command+Shift+T) opens or focuses the active lister's
  CLI in its own window. To do this in one click, add **CLI in its own window**
  under **View > Workspace > Customize workspace…**. **External terminal
  window** launches the separate terminal application configured in Settings.
- **Open with:** open one file with another application, or change the Haiku default for that file type.
- **Properties:** inspect metadata and explicitly save supported local typed
  attributes or ordinary permissions; opening the window alone changes nothing.
- **Workspace defaults:** All workspaces start in Details. Change each default in **Listi > Settings**. A remembered
  folder view takes priority. **View > File view > Use workspace default**
  clears the current folder and panel overrides.
- **Settings:** show/hide file icons, Size/Modified columns, the action button
  bank and the command field where the workspace exposes them; choose list font,
  file/folder/link colours, editor and terminal applications.
- **Command field:** type a command and press Enter to run it in a separate
  terminal, starting in the active local folder. For example, `pwd` prints
  that folder’s path. The shell stays open after the command finishes.
  Showing the field does not start a shell or run anything.
- **Operations:** reopen the progress window after hiding it; pause/resume,
  cancel and choose Replace/Skip/Abort when conflicts occur. For a local copy
  between regular files, **Keep both** preserves the original and gives the
  incoming file the proposed numbered name. If another program claims that name
  first, the copy fails without replacing it. During publication the new name
  can be visible before the copy finishes. If interrupted, consult the job
  report and recovery inventory before using a partial copy.

### Custom workspaces

Open **View > Workspace > Customize workspace…**, or go straight to **Modules** with **Listi > Settings > Workspaces > Customize current workspace layout…**. The four built-in presets
still work without changing any settings.

- **Action buttons:** remove buttons you rarely use and move the others into
  your preferred order. Removing a button does not remove its menu command
  or keyboard shortcut.
- **Modules:** place the button bank and command line above or below the panels,
  position the folder trees and name filters, or move the preview before or
  after the file panels. Arrange the panels side by side or vertically. The
  preset still determines which tools are available. Enable compact buttons
  to reflow the bank in a narrow window; turn the option off to keep the
  standard grid. Resizing does not choose another folder or file view. Stacked
  panels with a preview and both toolbars need more vertical space; side-by-side
  panels are a better fit for shorter screens.
- **Askr and Smiðja sidebars:** right-click a sidebar or open **Sidebar** in the
  panel's display menu to choose **File tree**, **Shortcuts**, **Shortcuts
  above tree**, **Shelf**, **Recent folders**, or **Hide sidebar**. To bring a hidden sidebar back
  in its last mode, right-click an empty area of the file panel and choose
  **Show sidebar**. Choices are remembered separately for each sidebar.
  **Recent folders** lists successful visits in this panel during the current session, most recent first. Click a location to return to it.
  Right-click a folder in the file panel or tree and choose **Add to shortcuts…**.
  Right-click a shortcut in the sidebar and choose **Remove shortcut**, or
  right-click it under **Go → Places**. Both use the same saved destinations.
  The **Shelf** sidebar shows the same references as the Shelf window. Drop
  files onto it, or right-click to add the selected files, remove a reference
  or open the Shelf window for transfer, archive and collection actions.
- **Smiðja preview:** select a file to inspect an image, plain text, source
  code, Markdown or HTML source, a PDF page, or audio/video supported by the
  installed decoders. Use the page buttons for PDFs or **Play preview** for a
  media sample of up to 15 seconds. Selecting media does not start playback.
- **Profiles:** name the current draft and choose **Create**. Later, select a
  profile and choose **Load** to use it or **Update** to replace its saved
  layout. Profiles remember layout, not open directories or selected files.

**Save** applies the draft and its profile changes together. **Cancel** or Escape
discards both. **Reset layout**, followed by **Save**, restores the standard layout
without deleting named profiles, folder-specific views, colours or application
choices.

### Archive conveniences

- Favourite formats appear directly; others are grouped under **More formats**.
  Choose a default, **Ask for a format every time** and a compression level per
  format where supported. Changing format updates the proposed extension.
- Choose an output folder and name. Extraction proposes a new “... extracted”
  folder rather than merging silently into an existing destination.
- AES-256 protection for supported ZIP/7z creation, with optional filename
  encryption for 7z.
- **Forget archive passwords:** explicitly clear the in-memory password cache
  and pending creation tokens; this does not delete decrypted working copies.
- **Resume archive edits**, **Save now**, **Open working folder** and **Start
  from current archive:** manage retained external-editor sessions. Decrypted
  working copies may remain on disk; read the recovery notes below.

## Inspect and edit a file

Choose the view that fits the task:

- **Listi > Quick view** (Command+Shift+P) shows the current selection next to the active panel in any workspace. In a two-panel layout, it temporarily replaces the other panel on screen. Move through the active list to inspect another file. Toggle Quick view off to return to the previous layout; the other panel has not navigated away. Space still selects files.
- From a file’s context menu, **Preview in other panel** (or **Preview beside file list** in a single-panel workspace) opens Quick view without toggling it off. Click **×** or press **Esc** to return to the file list. **Preview in window** opens a separate viewer.
- **View** (Command+Shift+V) opens a separate viewer for one file. Escape or Command+W closes it. **Listi > Inspector panel** keeps an inspector in a two-panel layout instead.
- **Edit** (Command+E) opens a supported local text file for editing inside the lister. You can also click **Edit** in its preview. Use **Detach** to move the live draft into a separate editor window, or **Done** to return to the preview. For the configured external editor, use **Files > Edit in external editor**.

### Text, source code and unknown formats

Plain text, code, Markdown and HTML display as text; scripts do not run.
The panel previews the first 256 KiB of a long file; the separate viewer can
show up to 1 MiB. Neither can save a truncated preview over the original.

In the preview, choose **Edit**. For an unfamiliar format, **Try as text…**
opens it only if the complete file passes UTF-8 and binary-content checks.
The editor accepts writable local regular files up to 256 KiB, including any
UTF-8 byte-order mark and line endings. Read-only files, files with set-user-ID
or set-group-ID permissions, symbolic links, archive members, network entries
and files with mixed or classic CR endings cannot be edited here. Archive editing has a separate
[save-back workflow](#editing-an-archive-member).

The editor colours recognised code, markup and Markdown without changing the
file. Above 128 KiB or 4,096 colour runs it uses plain text to keep editing
responsive. Use **Search > Find and replace…** (Command+F) for literal text,
**Find next** (Command+G), or **Go to line…** (Command+L). **View > Wrap lines**
changes the display only. Undo and redo are available in the Edit menu.
An asterisk in the window title marks unsaved changes. **Save** (Command+S)
preserves the original UTF-8 marker, LF or CRLF convention and BFS attributes.
Closing with edits offers **Discard**, **Cancel** or **Save**.

If the source was replaced or its bytes changed, Listi refuses the save and
keeps the edit open. Copy your edited text somewhere safe before reopening
the file. Listi writes a private original-byte recovery file in the same
folder before changing the original node; the folder must permit creating it.
If restoration cannot be verified, the error gives its path. After an
interrupted save, look for a hidden file beginning with
`.Listi-text-recovery-` in that folder. Compare it with the source before
restoring anything. This is not a version history or a lock against every
application that already has the file open.

### Photos, PDF and media

- **Photos:** open **Image…** for **Fit to window**, **Actual pixels (100%)**, **Zoom in** or **Zoom out**. Scroll over the image to zoom; drag it to pan. These controls do not crop or resize the original file.
- **Camera information:** for a JPEG with recognised EXIF tags, **Image… > Camera details…** can show the camera, lens, capture time and exposure settings. **Show GPS coordinates…** is a separate, explicit choice when location tags are present. The coordinates do not appear in ordinary camera details; no map or network lookup is used to interpret the tags.
- **PDF:** use **Previous** and **Next** to inspect individual pages.
- **Audio or video:** **Play preview** plays a sample of up to 15 seconds using installed Haiku codecs. Choose **Stop preview** to stop early. For a local file, **MediaPlayer** opens the full Haiku player instead. Selection alone does not play or launch anything.

## Panels and jobs

The highlighted panel is the source. Direct Copy or Move uses the other panel
as the destination; clipboard Paste uses the active panel. Accepted jobs retain
their selected paths and target even if you navigate or switch workspace.

In a writable local folder, use ordinary **Paste** (Command+V in the file
panel or **Paste here** in its context menu). File references keep their
copy/cut behaviour. For copied text or an image the filename is already
selected: type e.g. `Listi-forslag.md` and press Enter, or keep
`Clipboard.txt` / `Clipboard.png`. Escape cancels. The prompt freezes the
contents when opened and never replaces an existing file. Pasting in an
editable text field still inserts text into that field.

Open **Operations > Details** and choose a job by
number to review its result, even after another job has started. **Recent
activity** is the combined log; a specific job shows its source and destination
paths where available. Large reports have multiple pages, with errors first.
Listi keeps temporary reports for the newest 24 finished jobs in this window.
They are not saved across sessions. **Clear finished** releases the reports
without cancelling jobs or removing recovery records.
**Listi → Review interrupted operations…** shows recorded paths without touching
the affected volumes. Choose **Check local paths** for an optional identity
check. You can close the review while a check is pending. It never resumes
the job or deletes a file.

**Moving between local volumes:** Listi copies and checks each root before
moving its intact original into the source volume's Trash. If the source
volume has no supported Trash, use **Copy** instead. On removable media,
remount the destination before verifying important copies. **Compare folders**
in SHA-256 mode checks file contents, not permissions or BFS attributes.
Move within one volume keeps its usual behavior.

For a local move, **Moved** means Listi committed the move. **Copied; source
retained** means the destination copy was made but Listi did not remove that
source entry. **Published; directory flush failed** means the target was
changed, but its persistence after restart is unknown. Compare the target with
the retained source before retrying. **Partially completed** means some changes were made inside the
folder: check its individual entries. **Skipped** identifies an entry passed
over, **Failed** includes the problem, and **Not processed** is not a success.
Other providers can give a job-level error or a recovery path rather than
confirm the state of every file. Concurrent external edits are not prevented.

**Cancel job** requests cancellation of the running job only. In Details you
can cancel a selected waiting job, or choose **Cancel all** to discard waiting
jobs and request cancellation of the running one. A cancellation request does
not mean an in-flight write has stopped, and it does not undo completed work.
See the [virtual USB result](VERIFICATION.md#0361-3-package-results) and the
[physical USB test, not yet run](VERIFICATION.md#physical-usb-disconnect-during-copy).

In the development version, cancelling an upload does not start another
download to verify or remove temporary files. If the old destination has
already been renamed, its contents may be at the reported backup path rather
than the original name. Keep the local source and check the destination and
reported recovery paths before restoring, deleting, or retrying. A failed
SMB upload also leaves any uncertain files for manual recovery: Listi cannot
set an overall time limit on the SMB rollback calls.

If Listi stops unexpectedly during a local copy or move, compare the source
and destination before retrying. A hidden `.listi-copy-*` sibling in the
destination folder can be an unfinished file, not a backup. Other entries
may already be at their final names. Do not delete a source, or restore or
delete a staging file, based on its name alone. Verify the file contents and
any BFS attributes you need. Local copies report a zero-length typed
attribute as an error rather than silently omitting it.

Trash and permanent deletion are separate actions. Trash restore does not
overwrite an existing file and requires the original parent folder to exist.
Reinstalling an older application package does not undo file operations.

## Navigation, selection and native tools

**Select > By pattern** previews the number of visible
names matching a case-sensitive shell pattern, for example `*.pdf`. It
includes matching files, folders, links and visible dotfiles, never the
parent row. Choose **Select** to replace the panel selection, or **Cancel**
to keep it. Zero matches leave the selection alone. Changing the panel or
its listing invalidates the preview. This command neither changes the
literal name filter nor searches inside folders.

Panels offer details, icon and thumbnail views, history, saved places and optional
folder trees. Switching workspace retains the panel objects rather than
restarting file work. **Select same file type** replaces the active panel's
selection with visible ordinary files sharing an extension; it is not MIME or
content detection. `.jpg` and `.jpeg` remain distinct.

**Shelf:** Select local entries and press Command+Shift+Y
(**Files > Add to Shelf**), or right-click the selection and choose **Add to Shelf**. **Copy path as text** copies the address rather than file references; multiple paths are separated by newlines. Open **Listi > Show Shelf** to see their names,
original folders and status. You can also drop local entries onto the list.
Select one or more rows and click **Remove from Shelf** to remove just those
references. **Actions > Clear Shelf** removes every reference. Neither action deletes or
moves the original files. Closing and reopening the Shelf keeps its contents;
quitting Listi clears the working Shelf.

Use **Collections > Save current Shelf...** to keep a named snapshot of the
references. **Collections > Load** restores a snapshot to the working Shelf;
it does not start file jobs. Changes to the working Shelf are not written into
the saved snapshot until you save again and confirm replacement. Deleting a
collection only deletes its saved references.

On BFS, load and **Refresh** check volume and file identity. They can find a
rename in the known parent directory or use the volume's indexed name search
after a same-volume move that kept its name. Automatic lookup is bounded and
does not search other volumes or assume a replacement at the old path is the
original. A move that also changed the name, an unavailable volume, or an
unverifiable reference can require manual attention. Select one unavailable
row and choose **Relink...** to point it at a file or folder you recognize,
then save again to keep that change. On filesystems without a supported
identity proof, reloaded paths require explicit relinking.

In the Shelf window’s **Actions** menu, **Copy all...** and **Move all...** act on the whole Shelf, not just
highlighted rows. Choose a destination folder and follow progress in
Operations. Copy leaves the originals and the Shelf references in place.
Relink or remove unavailable references before submitting another job.

**Actions > Create archive...** opens the archive dialog for the whole Shelf. Choose
the destination, archive name and format. Use **Change format...** if the
format options are hidden. Each entry appears at the top level under its
original name. If two entries have the same name, remove one reference
before packing. Creating an archive leaves the originals and Shelf unchanged.

Local drag-and-drop asks Copy here, Move here or Cancel for the receiving
folder shown in the confirmation. In Details, icons and Miller columns, drag
over an outlined local folder to target it directly. Empty space or an entry
without an outline targets the current panel or column folder. Links, archives
and the parent entry are not subfolder targets. Duplicate and symbolic
link creation are local, single-entry actions; links can be relative or absolute.
Archive and network drops remain copy-only.

**Convert here:** Drop one local image onto a local panel,
choose **Convert here**, select a format and confirm **Convert**. The choices
depend on the image and the installed Haiku translators. Listi creates a new
file in the receiving folder and won't replace the original or an existing
output file.

**Bulk rename:** Select two or more entries in one local
folder and choose **Files > Bulk rename...**. Enter a prefix, suffix, case-sensitive replacement or sequence number, then
choose **Update preview** to compare current and proposed names.
Rules preserve extensions. For a swap, select each row, type its complete new
name and click **Set name**. The preview permits a name another selected row
will release, but rejects duplicate outputs and occupied, unrelated names.
**Apply rename** submits only the checked preview. An interrupted batch tries
to restore its original names; review Operations for any retained entries.

With the queue idle, use **Files > Undo last bulk rename**. Undo rechecks
identities and contents and refuses changed entries. A batch too large or
unreadable to verify has no undo. Haiku's rename API cannot atomically exclude
a competing writer between the final destination check and the rename.

**Files > Properties** pins one local entry and exposes timestamps, numeric
ownership, ordinary octal permissions and supported typed BFS attributes. Saves
are explicit. System namespaces, unsupported binary data and metadata beyond the
editor's limits remain read-only; this is not a MIME editor or bulk attribute tool.
Detected concurrent changes block editing, but checks cannot lock out all other
programs or guarantee power-loss atomicity.

![Local Properties with typed attributes and explicit save controls](screenshots/listi-properties-0.10.0.png)

*Properties, captured in 0.10.0 with synthetic data; cropped to the native window.*

**Listi > Add to Haiku file panel favorites…** adds the active local folder to
the favorites in Haiku Open and Save panels. Choose another name if one is
already taken. This does not modify Listi Places.

**Files > File type and icon…** opens Haiku FileTypes for one selected local
file or folder. Use it for that item's icon, type or preferred application,
rather than changing the default for every file of its type. Select the
original item, not a symbolic link.

**Files > Open with...** offers installed native applications for one local
regular file. **Open once** does not change associations. To change what Haiku uses for other files of the displayed type, select an application and choose **Set type default**. This changes the Haiku association, not just Listi, and does not open the file. A file with its own preferred application keeps that override. If the type is unknown, use **Open once** instead. Remote/archive members
must be copied locally for that dialog; ordinary archive Edit has its own
save-back workflow described below.

## Filtering and search

A panel's name filter is a local view filter, not recursive search or shell
wildcard expansion. Hidden rows are deselected; Select All and file operations
act on visible selections. New navigation clears the filter; refreshing the same
folder preserves it.

Click a Flat View column heading (Name, Subfolder, File type, Date modified, or Size)
to sort; click it again to reverse the order. File type groups filename extensions,
with — for files without an extension. Equal values are ordered by full path.

Choose **Flat View** in the panel’s view selector to show a recursive snapshot in the
active file panel. The other panel stays as it was. The **Subfolder** column distinguishes
identical filenames; `.` means the starting folder itself. Filter by a literal
name or relative-folder fragment without rescanning. Filtered-out files are
deselected. Choose **Refresh** to rescan the same folder with the filter intact.

Select files and click **Add to Shelf** to collect references to those exact
originals. If an original is unavailable or has been replaced since the scan,
the selection is not added. Double-click or press Enter to open a result. You can also
select multiple rows and use the file clipboard or copy them to the opposite panel.
Select a row to show its original in the Inspector.
Choose the previous view in the panel’s view selector to return to its file list and selection. To scan another folder, navigate there and choose **Flat
View** again.

The scan runs in the background and lists regular files on the same persistent
local volume. It does not follow symbolic links or enter Trash. **Stop scan**
keeps the rows received so far. A stopped scan, unreadable branch or scan limit
(5,000 results or 250,000 entries examined) is reported as incomplete.

In **Smiðja**, **Listi > Find files** (Command+Shift+F) opens **Search** in the active panel. Enter a name fragment and press Enter. Click **Files** to return to the same file list, then **Search** to resume without restarting. **Quick view** opens **Preview** in the opposite panel. Close a tool with **×**; closing the last tool removes the tab row. Outside Smiðja, Find files still opens its own window.

**Listi > Find files** combines a name fragment or BFS formula with extension,
size and date criteria. Saved searches store criteria, not copies of results.
Advanced BFS queries can subscribe to live changes. Search results refer to
original files: deleting a result's file deletes the original. Right-click a result and choose **Show original in panel** to reveal that specific entry.

Search is currently limited to one persistent local volume, 5,000 displayed
hits and 250,000 examined entries, with explicit incomplete/error outcomes.
Content, archive-internal, remote and multi-volume search are not supported.
Date filters apply to current files, not historical snapshots.

![Search with extension, size and date criteria](screenshots/listi-search-filters-0.30.0.png)

*Find files, captured in 0.30.0 with synthetic fixtures.*

## Folder comparison

**Listi > Compare folders...** compares relative names and types recursively.
Size mode does not verify equal content; SHA-256 mode reads content on both
sides. Comparison is read-only: it does not repair or synchronise anything.
Permissions, ownership and BFS attributes are not compared.

Keep both folders idle for meaningful results. Comparison is not a snapshot;
remote checks depend on the server's metadata, and hashing can download entire
files. Limits are 100,000 entries per side, 256 levels and 5,000 displayed rows;
truncation and incomplete scans are reported.

![SHA-256 comparison distinguishing equal-size files with different contents](screenshots/listi-folder-compare-0.28.0.png)

*Comparison, captured in 0.28.0; the selected files have equal size but different content.*

## Archives

Available formats depend on optional system libraries and engines. Listi can
browse many formats, but reading, creation and editing are different
capabilities. A successful listing does not establish member-data integrity.

- Supported local single-volume writers include ZIP, 7z, TAR, TAR with gzip,
  xz, bzip2, LZMA, lzip or Unix compress (.tar.Z), CPIO and XAR. Zstandard/LZ4 wrappers are gated by the
  installed engine rather than universally promised. Reading support is broader
  than writing support.
- Unix ar archives support flat regular files. Edits preserve GNU or BSD
  naming. Debian packages and static libraries are not rebuilt as generic ar;
  thin archives with external references are not opened.
- With p7zip installed, Listi can create single-volume ARJ archives with
  portable ASCII names. Install the optional HaikuPorts `arj` package for
  compression. Level zero (or no compressor installed) uses Store. Listi
  can edit archives with its portable header layout. Compatible DOS/Windows ARJ archives can also be read, including
  compressed members; other header variants are not assumed editable.
- LHA/LZH can be created and edited when the optional HaikuDepot `lha` tool
  and libarchive are installed. Level zero stores files; other levels use LH7.
- Single-volume Microsoft Cabinets support Store at level zero and MSZIP
  compression at levels 1 to 9 when system zlib is available.
  Nonempty folders are represented by their member paths. CAB cannot store an
  empty folder; an edit that would leave one is refused without replacing the
  archive. Signed, spanned and unsupported Cabinet variants are not editable.
- Listi can create ISO 9660 data discs. It verifies the member names, contents
  and permissions and records the image digest in a filesystem attribute.
  Only images with that matching attribute can be edited. Other images
  remain browseable where supported; they are not rebuilt as plain data discs.
- Writable archive folders support Copy/Paste, new folders, rename, entry removal
  and supported moves. Removing an archive member is not local Trash. Editing
  rebuilds the container and needs scratch space for extracted content and the
  new archive; layout, comments and format-specific metadata may change.
- Supported encrypted ZIP, 7z and RAR inputs use a masked password flow.
  Passwords are kept in process memory rather than saved in settings.
- Selected local multipart 7z and RAR naming families are supported for reading.
  All parts must remain together and unchanged. This does not imply split ZIP,
  split creation or repair support.
- Supported local archive-to-archive and archive/local moves within the same volume verify destination
  contents before source cleanup. They are not an atomic two-location
  transaction and have no automatic crash-resume journal.
- Archive/local moves refuse unsupported metadata and file types rather than
  silently discarding them, including BFS attributes, source symlinks, special
  files and some hard-link cases. Use Copy to retain originals.
- With the p7zip RAR5 reader installed, Listi writes single-volume RAR5 using
  Store (no compression). Ordinary existing RAR5 files, including supported
  compressed members, can be edited if their metadata can be retained.
  Rebuilding may increase the file size. RAR4, encrypted or multipart RAR,
  recovery records and unsupported metadata are not writable.
- Remote, split and nested archives cannot be edited in place.

A failed or cancelled merge may leave destination copies, and cleanup may stop
after some verified entries have moved. Inspect both locations before retrying.

### Editing an archive member

Select a regular member in a supported writable local archive and choose **Edit**.
Listi opens a private working copy in the configured external editor. While the
edit window remains open, stable editor saves can be written back through a
verified archive rebuild. **Save now** retries a paused save; external archive
changes stop automatic saving rather than being silently overwritten.

![Archive edit window after verified save-back, with retained working copy](screenshots/listi-archive-saveback-0.27.0.png)

*Archive save-back, captured in 0.27.0 with a synthetic text file.*

Working copies and recovery records remain under
`~/config/settings/Listi_archive_edits/edit-*/`. **Decrypted contents are not
encrypted at rest.** Keeping a copy, closing or cancelling does not remove it.
Use **Files > Archives > Resume archive edits** to resume a retained session.
After closing edit windows and preserving needed work, remove obsolete copies
manually. This is not write-back from arbitrary Open or Open With applications.

## Network locations

SMB, SFTP, FTP and FTPS use the panel and job interface. Capabilities and recovery
behaviour vary by protocol and server. Comparison or previews can cause network
reads, including downloading complete files. Keep credentials out of shared URLs,
screenshots and bug reports.

A network copy transfers file contents, not Haiku BFS attributes. Downloads
apply available portable permissions and modification time. Uploads verify the
staged file contents before publication; an upload is not a promise to retain
source permissions, timestamps or other metadata on the server. Remote
progress can show the current file and bytes without a total.

For SFTP, open **Connect** and enter the address and username. Under **Advanced**,
**SSH private key** defaults to the first readable `id_ed25519`, `id_ecdsa` or
`id_rsa` in Haiku’s `~/config/settings/ssh/` directory, then `~/.ssh/`. For a custom login key, enter
its path (not its `.pub` file). For an encrypted key, Connect opens a masked passphrase prompt. You may also
fill **SSH key passphrase** under Advanced. Do not put it in **Password**. The passphrase is used locally to
unlock the key; it is not sent as the server password or saved to disk.
**SSH known hosts** identifies the server: keep the default if it already contains
a trusted key for the address you entered, or select a known_hosts file obtained
from the server administrator. An unknown or changed host key stops the connection
before your login key is checked. Verify the server fingerprint before trusting
a new key.

## Settings and older versions

The 0.31.1 naming update retains existing workspace values and settings aliases.
Saved search criteria written since 0.30 use a newer store format: 0.29 and older
cannot read it. Those versions preserve the file and disable saved-search changes;
upgrading restores access. Do not replace the store with a stale backup merely
to make an older version read it.

### BFS columns and searchable attributes

In Properties select an attribute and click **Show in Details**. The originating
pane switches to Details and displays that attribute for each ordinary local
file or directory. Choose up to four columns per pane for the session. This
does not change remembered folder views or the other pane. **Clear attribute
columns** in the pane's view selector removes them and restores the regular
view. Missing and empty values have distinct labels. In ascending order,
unavailable values sort before numbers, which sort before text. Numbers sort
by value; numeric-looking strings remain text. Long values are previews, not
editors.

**Make searchable…** names the volume before creating an index there. This is a
volume-wide action, not a change to the current folder. Listi supports the BFS
index types string, signed 32/64-bit integer, float and double. It checks an
existing index instead of replacing it, and checks whether a native query
returns the selected entry. Creating an index does not rewrite or backfill old
attributes. Use Find files' volume query mode to search an indexed attribute.

### Use a local search as a file list

In **View > Find files…**, choose the search folder and criteria, or load a
saved search, then choose **Open as file list**. The query replaces the file
list in the originating pane; **Back** returns to the previous location.
Each row refers to its original file and shows its original folder in Details.
Use normal multiple selection, Copy, preview or Add to Shelf. For a live BFS
predicate, additions and removals update the list without transferring a
selection to a replacement file. An unavailable scope is shown as an error;
Reload retries the same criteria.

A search is not a physical folder. To paste or drop files, navigate to an
actual destination folder. Opening a saved search reruns the criteria; no
files are copied into the saved search.

### Folder tabs

Each file pane has its own folder tabs, separate from the Files, Search and Preview tools in Smiðja. **Window > Folder tabs** opens a new tab in the selected folder (otherwise here), duplicates a tab with its current state, renames or moves it, closes it, or reopens the last tab closed in that pane. A single tab stays hidden unless **Always show folder tabs** is selected. The last file tab cannot be closed on its own; close the window instead.

While browsing files, use **Command+Option+T** for a new tab, **Command+Option+[** and **Command+Option+]** to cycle, and **Command+Option+W** to close a tab. Command+T and Command+Shift+T still open the terminal. The tab strip is for navigation, not a file-drop target: drop onto the file view of a real folder. Switching tabs does not redirect an already queued copy.

**Detach to Einn window** moves the active tab, including its navigation history, filter, view and selection. If you detach the only tab, the original pane opens your home folder. The new window keeps its own queue and uses session-local layout preferences; its layout does not change the initial browser’s saved workspace. Closing the original window does not close a detached file window. Folder tabs last for the session and are not saved worksets.

### Independent file windows

Choose **Files → New window** (Command+Shift+N) to browse the active folder in another native window. The new browser uses the saved workspace layout and starts its own history, selection, filter and operation queue; Command+N still creates a folder. Choose **Open in new window** from Files or a folder’s context menu to start in the selected folder. The windows can show the same location, and Haiku’s usual Stack & Tile controls work on them.

From Terminal, use `/boot/system/apps/Listi --new-window [directory]`. Without a directory it opens the default location. In Preferences, the separate-window launch option controls whether launching Listi again from Tracker or Deskbar opens a browser; selecting an existing window only activates it.

**Close window** (Command+W) closes that browser; **Quit Listi** (Command+Q) requests application-wide shutdown. Close a browser’s CLI sessions and finish or cancel its file operations first. Closing the last browser normally quits Listi. Additional browsers keep layout changes for the session; global preferences, Places, and saved searches are shared.

### Export a file list

In a Files panel (including a search list), choose **Files → Export file list…**.
Choose selected or all visible items and review the count and first three rows.
Both choices use the filtered order and values captured when the window opened.
Save to a new filename; existing files are not replaced. Import the CSV as UTF-8
with comma separators and double-quote text delimiters. Times are UTC; unknown
values and folder sizes are empty. To prevent formula evaluation, exported text
starting with a tab or line break, or whose first non-whitespace character is
`=`, `+`, `-` or `@`, receives a leading apostrophe. This changes only the CSV.
