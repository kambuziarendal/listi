# Listi

Listi is a native Haiku file manager. Browse a single folder, compare two panels side by side, or use the Smiðja workspace to keep a file list, search results and a terminal in the same window.

![Listi showing a photograph beside its file list](docs/screenshots/listi-quick-view-0.35.0.png)

*Quick view in Listi 0.35.0. [Image credit](docs/screenshots/CREDITS.md).*

## Install

Download the **[0.37.1-2 alpha test candidate for Haiku x86_64](https://github.com/kambuziarendal/listi/releases/tag/v0.37.1-2)**. Open the HPKG in HaikuDepot, or install it from the download folder:

```sh
pkgman install ./Listi-0.37.1-2-x86_64.hpkg
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
pkgman install haiku_devel python3 libarchive_devel zlib_devel curl_devel samba4_devel
make
```

Run `make test` for the built-in self-test or `make package` to create an HPKG in `dist/`. See [testing and known limits](docs/VERIFICATION.md) for the full native audit.

[Changelog](CHANGELOG.md) · [Roadmap](docs/ROADMAP.md) · [Report a bug](https://github.com/kambuziarendal/listi/issues) · [MIT licence](LICENSE) · [Third-party licences and the RAR restriction](docs/LICENSING.md)
