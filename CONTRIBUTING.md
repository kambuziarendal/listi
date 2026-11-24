# Contributing

Listi is a native Haiku development alpha. Small, focused changes with clear
behaviour and reproducible tests are welcome.

## Keeping changes small

Before adding a file action, trace it from its UI entry points to the operation
that owns it. For example, a new way to start a move should reuse the existing
move and conflict handling, not grow its own copy. If the existing boundary no
longer fits, extract the smallest useful piece and test the old callers before
switching them. Keep unrelated cleanup out of the feature diff.

If a change requires several subsystems to switch at once, lay out compatible
steps first. Each step should build, preserve existing workflows and be
reviewable on its own.

Before review, run `git diff --check BASE HEAD`, where `BASE` is the
commit your change builds on. For C++ changes, if clang-format and its Git
helper are installed, use `git clang-format --diff BASE HEAD` to inspect
formatting on the changed lines without rewriting unrelated code. These
checks do not replace native compilation or regression tests.

## Bugs and testing

For a release package, include the tag and whether its checksum matched. Include the Listi and Haiku versions, reproduction steps and whether files were
affected. Use disposable fixtures and independent backups when testing file
operations. Never attach passwords, private files or personal filenames to a
public issue. For the inspector, say whether you used Quick view, the panel
inspector or the separate viewer. For an editor save issue, note the file
encoding and line endings if known. For a photo issue, use a synthetic or
openly licensed example without personal GPS tags. See the
[testing guide](docs/VERIFICATION.md).

### Bug hunts: test the transition, not just the feature

For each affected user journey, use the installed application and follow at
least one realistic sequence **without resetting the window between steps**.
Include the relevant non-happy path (for example Discard, conflicting files
or unsupported metadata), then perform the next ordinary action. Verify both
the resulting data and what the user actually sees. A successful exit, a
correct filename or a responsive window does not prove the preview rendered.

- For Inspector: capture the initial text preview, edit and discard, select a
  known PNG, switch back to the same text and hide/show Inspector. Compare
  content, margins and scrollbars at the same panel width, and verify the file
  on disk did not change. Use real menu/pointer/keyboard input for the path
  being evaluated; scripting that bypasses the control is not UI evidence.
- For removable media: use representative files **with** native attributes,
  not just plain bytes. Check the source, destination and visible job outcome,
  including a successful job after a failed one. Distinguish VM coverage from
  physical hardware and explicitly list any untested combination.
- When a user finds something a hunt missed, retain the shortest sequence and
  a failure criterion in the relevant regression set. Report which steps were
  actually observed; do not extrapolate from an isolated feature test.

## Public documentation

- Write documentation, issue summaries and release notes in English. Product
  names and third-party credits retain their original spelling.
- Say how a result was obtained: distinguish hands-on hardware testing from
  automated VM runs, and identify LLM assistance where relevant. Keep private
  conversations, internal approvals and raw tool logs out of public reports.
- Keep local machine paths, private service addresses and internal acceptance
  logs out of public documentation and release assets.
- Distinguish what is in the source tree from what is in the downloadable
  package and from proposed features. Link a release by tag and use the exact
  HPKG asset name in installation examples.
- Use real, version-labelled screenshots, descriptive alternative text and
  attribution for the images they contain. Preserve reproducible tests and
  useful limitations alongside feature descriptions.
- Keep documentation links and package staging valid when moving or removing
  files. Keep implementation changes separate from editorial cleanup.
