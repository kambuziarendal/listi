# Listi

Listi is a native Haiku file manager. Browse a single folder, compare two panels side by side, or use the Smiðja workspace to keep a file list, search results and a terminal in the same window.

![Listi showing a photograph beside its file list](docs/screenshots/listi-quick-view-0.35.0.png)

*Quick view in Listi 0.35.0. [Image credit](docs/screenshots/CREDITS.md).*

## Feature overview

- **Browse your way.** Choose one of four workspaces, use per-panel folder tabs or detach a tab into its own window. Open a live BFS search as a file list, or use Flat View to sort an entire local tree by subfolder, type, size or modification date.
- **Work with a selection.** Queue transfers, keep both files on a local copy conflict, copy names or paths as text, export a displayed list to CSV, or collect originals from several folders on the Shelf. Compare folders by SHA-256 without changing either side.
- **Stay in the file manager.** Preview photos, PDFs and media, edit text, work with typed BFS attributes, detach a running shell, or browse archives and SMB, FTP, FTPS and SFTP locations.
- **Return to an earlier version.** On local BFS, save versions by hand or turn on the optional Deskbar helper for selected folders. Open the folder’s timeline in another window or the opposite panel, then restore one file or a whole folder as a new copy.

The [feature tour](docs/FEATURES.md) covers Mini icons, compact Einn windows, working sets and the other tools included in this alpha.

## Install

Download the **[0.38.0-22 alpha test candidate for Haiku x86_64](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-22)**. Open the HPKG in HaikuDepot, or install it from the download folder:

```sh
pkgman install ./listi-0.38.0-22-x86_64.hpkg
```

This is alpha software. See the release notes for what was tested, and try it on copies of your files. To transfer files between disks, use **Copy**, remount the destination and check the copy before removing the original. In 0.36.1-3 a virtual USB disconnect could leave a regular-file copy waiting after **Cancel**. Newer candidates isolate staged data writes and flushes on USB destinations so a stalled request cannot later publish the file. The [physical USB test](docs/VERIFICATION.md#physical-usb-disconnect-during-copy) has not been run.

## Basic workflow

Choose **View > Workspace**, open the source and destination folders, then select files in the source panel. **Quick view** previews the selected file without opening another application. In Smiðja, use **Show original** on a search result to select it in the file list.

See the [feature tour](docs/FEATURES.md) for screenshots or the [workflow guide](docs/GUIDE.md) for shortcuts, archives and network locations.

## Development and testing

The developer uses large language models (LLMs) to help write code and design and run tests. Much of the hands-on testing takes place on a ThinkPad X1 Carbon (6th Gen) running Haiku R1/beta6 directly on the hardware, not in a VM. Much of the automated stress testing runs in Haiku VMs with LLM assistance. The [verification notes](docs/VERIFICATION.md) distinguish test environments: the mid-copy disconnect result is from a virtual USB device.

## Build from source

On Haiku x86_64:

```sh
pkgman install haiku_devel python3 libarchive_devel zlib_devel curl_devel samba4_devel sqlite_devel
make
```

Run `make test` for the built-in self-test or `make package` to create an HPKG in `dist/`. See [testing and known limits](docs/VERIFICATION.md) for the full native audit.

[Changelog](CHANGELOG.md) · [Roadmap](docs/ROADMAP.md) · [Report a bug](https://github.com/kambuziarendal/listi/issues) · [MIT licence](LICENSE) · [Third-party licences and the RAR restriction](docs/LICENSING.md)
