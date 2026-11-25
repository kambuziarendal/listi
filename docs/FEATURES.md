# A tour of Listi

Listi can be a small folder window, a two-panel copying desk, or a workspace with a file list, preview and shell. Start with one task; you don't need to configure a workspace to use it.

[Install the Haiku x86_64 alpha](../README.md#install) · [What's new in 0.38.0-23](../CHANGELOG.md) · [Workflow guide](GUIDE.md)

[Browse](#browse-in-a-window-that-fits) · [Find and select](#find-and-select-the-right-files) · [Transfer](#copy-move-and-recover) · [Restore versions](#go-back-to-an-earlier-version) · [Preview and edit](#look-inside-a-file) · [Organise](#organise-files-across-folders) · [Archives and servers](#work-inside-archives-and-on-servers) · [Haiku tools](#use-haikus-tools-without-losing-your-place)

## Browse in a window that fits

Choose **View > Workspace**. Switching layouts keeps your locations, selections and accepted jobs.

| Choose | When you want to… |
| --- | --- |
| **Einn** | Open one folder in a compact window. Switch back to a larger workspace to recover its previous size. |
| **Askr** | Keep a folder tree or shortcuts beside one file list. |
| **Tvískipt** | Keep a source and a destination side by side. Tab changes which pane is active. |
| **Smiðja** | Browse two locations with independent sidebars and tools such as search, Inspector or a shell. |

**Choose what the folder shows.** Under **View > Active pane view**, use Details for sortable columns, Mini icons for names beside small icons, Icons or Large icons for a grid, Thumbnails for pictures, or Miller columns to keep ancestor folders visible. Remember a view for a particular folder without changing the workspace default. Details loads large folders in batches, so you can navigate before every row appears.

**Keep your place.** Each pane has its own Back and Forward history. Type a path, revisit Recent folders or choose a saved Place from Go. Duplicate, name or reorder folder tabs; reopen a closed tab at the tab strip. **Window > Detach tab to Einn window** moves the active tab to its own compact window. Open another browser with **Files > New window** or **Open in new window**. Name windows and switch among them in **Window > Open windows…**.

**Reuse a setup.** A *working set* reopens selected windows with their folders and tabs: preview its contents before adding those windows to your desktop. A *workspace profile* saves an arrangement of tools, not open folders. Customize buttons, panel orientation, filters and sidebars, or use **Help > Layout introduction…** to choose a starting layout. [Workspace controls](GUIDE.md#custom-workspaces) · [Window and tab behaviour](GUIDE.md#folder-tabs)

## Find and select the right files

| If you need… | Use | What you get |
| --- | --- | --- |
| A shorter list | **Filter this folder** | Visible names matching a literal fragment. Select All, None and Invert work on this list. |
| A group of similar files | **Select > By pattern…** or **Same file type** | Preview the count for a wildcard such as `*.png`, or select visible ordinary files with the same extension. Checkmarks in Details distinguish selection from focus. |
| One list of a local tree | **Flat View** | Sort by subfolder, name, type, size or date across nested folders. Stop a scan to keep partial results; this is not a live search. |
| Matches across subfolders | **Listi > Find files…** | Combine name, extension, size and dates. Save criteria, or use a BFS formula with Live updates. Open results as a file panel, preview a hit or use **Show original**. |

Type the start of a name to focus an entry; Space toggles selection. Filtered-out entries do not silently rejoin a selection. A search result refers to the original file, not a copy; the list is not a paste destination. [Filtering and search](GUIDE.md#filtering-and-search)

## Copy, move and recover

1. In **Tvískipt**, open the destination on one side and select files on the other. **Copy to other panel** (Command+Shift+C) uses the target shown at the button. For another window, **Files > Copy to window…** lets you choose its writable local folder. Accepted jobs do not follow later navigation.
2. On a name conflict, choose for **this file** unless you explicitly apply the choice to the rest. **Keep both** proposes a numbered name for a regular local copy. Dragging onto a highlighted local subfolder offers Copy or Move; clipboard Paste targets the active folder.
3. Read the one-line receipt. **Listi > Operations > Details** shows paths, results and which job failed. Pause/resume, cancel a running or waiting job, or cancel the queue. Cancellation does not undo completed files.

**Moving between disks:** Listi copies and checks a root before sending its original to the source volume's Trash. If no supported source Trash is available, use Copy. A normal copy to FAT can ask to omit unsupported Haiku attributes for that job and volume; Move never gets that exception. Verify important copies after remounting. An NTFS volume reporting “device busy” is not ready to unplug.

**Getting something back:** Trash Restore previews original destinations and does not replace existing files. Permanent deletion is separate. After a stopped job, **Review interrupted operations…** shows recorded paths without first probing a missing disk; it does not replay or delete anything. [Transfer results and recovery](GUIDE.md#panels-and-jobs)

## Go back to an earlier version

Open a local BFS folder and choose **Files > Folder history…**. Choose a dedicated BFS history folder outside the source and save a version. To save changed trees periodically instead, use **Automatic history settings…**: enable the helper for up to three source/store pairs. Its clock in Deskbar opens settings, even with Listi closed.

In a configured folder the version count is clickable. Right-click the background to choose a separate history window or a read-only preview in the other pane. Drag the timeline, use the arrows or scroll between dates. Additions, removals and changes are relative to the preceding version; browsing never restores anything.

Choose **Restore file…** for one regular file or **Restore folder…** for the whole saved version. Pick a containing folder and confirm the proposed unused name. There is no in-place rollback, merging or overwrite. The helper stops at its limits rather than deleting old versions; this is neither an atomic snapshot nor an independent backup. [Setup, limits and restore](GUIDE.md#automatic-folder-history-optional)

## Look inside a file

**Quick view** (Command+Shift+P) uses the space of the opposite pane without navigating it. **View > Inspector panel** keeps a preview beside the panes, while **Files > View** opens a separate viewer.

- **Images:** fit, zoom to actual pixels or pan. Read recognised JPEG EXIF data locally; showing GPS coordinates is a separate choice. Drop an image on a local panel and choose **Convert here** for a new file. JPEG turns transparency white; choose PNG to retain it.
- **PDF, audio and video:** turn PDF pages or play a short media sample with installed Haiku codecs. Selecting the file does not start playback. Open a local file in MediaPlayer for full playback.
- **Text and code:** Markdown and HTML are shown as source. Choose **Edit** for supported writable local UTF-8 files up to 256 KiB. Find and replace, jump to a line, wrap text, undo or detach the draft. Save checks for external changes; Discard leaves the original alone. **Edit in external editor** is a separate command.

[Preview controls and editor limits](GUIDE.md#inspect-and-edit-a-file)

## Organise files across folders

**Collect without copying.** Add local files from different folders to the **Shelf**, or drop them on its sidebar. Save a named collection, relink a missing original, or copy, move or archive the whole Shelf. Removing a reference does not delete its file.

**Name and create.** Right-click a writable folder's background for **New file** or **New folder**. Duplicate creates a copy; a symbolic link points to the original. For a batch rename, preview numbering, prefixes, suffixes or replacements, adjust individual names, then apply. Undo is available for the last eligible batch if its files still match.

**Compare and share a list.** **Listi > Compare folders…** compares names and sizes or reads contents for SHA-256. It shows differences rather than synchronising or deleting, and doesn't compare permissions or BFS attributes. **Copy as** puts names or paths on the clipboard; **Export file list…** writes selected or visible entries to a new CSV. Pasting clipboard text or an image into a writable folder can instead create a new text file or PNG. [Comparison](GUIDE.md#folder-comparison) · [CSV export](GUIDE.md#export-a-file-list)

## Work inside archives and on servers

**Archives.** Open a supported archive like a folder. **Files > Archives** creates an archive from a selection or extracts to a new folder. Change format and compression for one job, or save preferences separately. Supported ZIP and 7z creation can use AES-256; 7z can also encrypt names.

Writable local formats support selected additions, renames and removals through a rebuild. Edit a regular member in an external editor and resume its save-back session later. Decrypted working copies can remain on disk. Reading a format does not imply it is writable: examples include selected multipart archives (read only) and single-volume RAR5 (Store writer). [ZIP, 7z, TAR, RAR, ISO and other formats](GUIDE.md#archives) · [Archive editing](GUIDE.md#editing-an-archive-member)

**Servers.** **Listi > Connect to server…** opens SMB, SFTP, FTP or FTPS in a pane. Browse, preview or transfer against a local folder using the usual queue. Supply passwords separately from the URL; Advanced includes the SMB domain and SSH keys/known hosts. A second login cannot silently change the account used by an existing route. Plain FTP is unencrypted. Network copies do not carry BFS attributes; available timestamps, operations and recovery depend on the server. [Connections and transfer limits](GUIDE.md#network-locations)

## Use Haiku's tools without losing your place

**Properties and file types.** Inspect a local entry's size, timestamps and permissions, edit supported typed BFS attributes, or add an attribute column to Details. **Make searchable…** checks or creates a supported volume index; it does not backfill old values. **Open with…** can launch an app once or explicitly change the type default. **File type and icon…** opens Haiku FileTypes for the item. Add a folder to Haiku's Open/Save panel favourites independently of Listi Places.

**Terminals and preferences.** Start **Listi > Terminal here** in the active local folder. Keep separate left and right shells, move the dock or detach a running CLI without losing scrollback. Launch an external terminal or use the optional command field for a separate shell. Settings also control fonts, colours, columns, default views, action buttons and preferred applications.

[Haiku attributes and queries](GUIDE.md#bfs-columns-and-searchable-attributes) · [Tools and preferences](GUIDE.md#tools-and-preferences) · [Verification and known limits](VERIFICATION.md)
