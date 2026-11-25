# Qualifying a Haiku x86_64 package

See the [0.38.0-23 alpha release notes](https://github.com/kambuziarendal/listi/releases/tag/v0.38.0-23) for results specific to that package. The procedure below is for qualifying a build.

1. Choose the exact commit to qualify. Use a clean release checkout on Haiku;
   save `git rev-parse HEAD` and check `git status --porcelain` is empty.
   Record `uname -a`, compiler version, optional tools and package revisions.
2. Check the root Makefile version, PackageInfo version/revision, executable
   version resource, changelog and proposed tag agree. Release notes should
   distinguish installed features, missing optional tools and known limitations.
3. Run `make audit` on that checkout. Save its complete output and exit status.
   A focused subset, the smoke target, or a Linux build-tool check does not
   qualify a native package. Record skipped optional checks by name and reason.
4. Without changing the source, run `make package`. Check the HPKG filename,
   `package list -i <package.hpkg>` and `sha256sum <package.hpkg>`. Keep that
   artifact: building another container need not produce identical bytes.
5. Install that exact file with `pkgman install ./<package.hpkg>` in a
   disposable Haiku system or snapshot. Confirm `/boot/system/apps/Listi`
   matches the qualified build binary, check its version with `version` and
   run `/boot/system/apps/Listi --self-test`. Start the installed app outside
   the checkout, switch workspaces, and copy a disposable file using explicit
   source and destination panels. Compare both files to the expected bytes.
   Record installation, actual outcomes and any untested configurations.
6. For a release, pin the tag to the qualified commit and publish only the
   tested HPKG with its SHA-256, matching notes and test summary. Verify the
   downloaded bytes against the saved digest. A candidate without a release
   is not a new download; do not change the public download link prematurely.
