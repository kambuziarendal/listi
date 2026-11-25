# Usability review: 0.38.0-23

These 25 changes address findings from the earlier desktop reviews and hands-on feedback. The table describes implemented behaviour, not 25 independent physical-device passes. See the [alpha release notes](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-23) for the checks performed on the shipped package.

| # | What to try in the alpha |
| --- | --- |
| 1 | In a two-panel workspace, check the Copy or Move button for the destination before starting. |
| 2 | Resolve one conflict without selecting the separate option to apply the choice to remaining conflicts. |
| 3 | After a failure and a later successful job, open Details to identify the job, file and destination associated with the error. |
| 4 | Filter names in the current pane, then use Find files to search subfolders. |
| 5 | Open a configured history source after reboot. Its version indicator opens the correct store without entering a path. A manual history offers a folder picker. |
| 6 | Restore entries from Trash. Check their original destinations before confirming; occupied paths must not be overwritten. |
| 7 | Start New folder in either pane; the prompt identifies the containing folder. |
| 8 | Select a saved working set. Review its windows and folders without opening them. Opening the set keeps the current windows. |
| 9 | Create a ZIP using the ordinary name and destination. Change the format when needed; lasting format preferences are separate. |
| 10 | Reopen a closed tab from its pane's tab strip, or use Window > Folder tabs. |
| 11 | Compare confirmation prompts for one item and several items; check which names or count the action applies to. |
| 12 | Copy an attributed BFS file to FAT. Ordinary Copy asks before omitting Haiku attributes for that job and volume. Move retains its stricter rules. |
| 13 | Check the label on a failed row; a red mark alone is not the only indication. |
| 14 | In Quick view compare a screenshot at Fit and at 100%. Downscaling cannot preserve the legibility of every character. |
| 15 | Open Einn directly, switch from a larger workspace, or detach into Einn. The window starts compact; switching back restores the previous multi-panel frame. |
| 16 | Use Window > Detach tab to Einn window without going through the Folder tabs submenu. |
| 17 | Select same file type in Details. Selected items have a checkmark distinct from row stripes and keyboard focus. |
| 18 | Convert a transparent image to JPEG: transparent pixels are matted against white. Use PNG to preserve transparency. |
| 19 | Right-click empty space in a writable local folder and choose New file. An existing name is not replaced. |
| 20 | Choose Mini icons for small icons with the filename beside each icon. |
| 21 | Finish an ordinary copy: the success receipt fits on one line; Details still has the job record. |
| 22 | Restore a version to an existing parent folder. Confirm the suggested new path; restore never merges folders. |
| 23 | Select a regular file within a version to restore only that file; restoring the full version is a separate action. |
| 24 | Browse an older point in the opposite pane with the slider, arrows or wheel. Close it to return to the original selection. Browsing does not restore anything. |
| 25 | Edit text in Inspector, discard, select a PNG, switch back to text, then hide and reopen Inspector. Check the drawn image, text margins and original file. |

## Hands-on follow-up

On the target hardware, repeat the attributed FAT copy and the Inspector sequence above using disposable files. Compare the destination contents and the visible state, not just the status text. For a usability study, ask someone unfamiliar with Listi to predict a transfer destination, keep both conflicting files and restore one file without hints. Record wrong predictions separately from wrong clicks; repeat with keyboard-only input and a narrow window.

References: [Haiku Tracker](https://www.haiku-os.org/docs/userguide/en/tracker.html), [Haiku shortcuts](https://www.haiku-os.org/docs/userguide/en/keyboard-shortcuts.html), [usability heuristics](https://www.nngroup.com/articles/ten-usability-heuristics/), [GNOME feedback guidance](https://developer.gnome.org/hig/patterns/feedback/notifications.html).

## Open feedback from 0.38.0-30 testing

- [ ] Remove the “Inspector” heading inside both the docked panel and the detached window. Keep the detached window title.
- [ ] Keep “Select a file to inspect” when nothing is selected, aligned near the bottom edge with a small inset.
- [ ] Use Listi’s normal panel background instead of white. Outline the docked Inspector with a thin frame.
- [ ] Make the upper-right button more compact in both the docked Inspector panel and the detached Inspector window.
- [ ] After physical USB removal during Copy, dismissing all visible errors left the file red with “Needs attention”. Make it clear whether an action is still pending or only an acknowledged failure remains.
- [ ] Beta blocker: invalidate disconnected-volume panes and prevent new scans and opens against stale entries. In the physical -30 test, screenshots remained viewable after unplug; navigating to another folder then ended in the Haiku kernel debugger. This does not establish that reads reached the removed device.
- [ ] Add “Open in Listi” to the Tracker context menu for folders without changing the system file associations.
