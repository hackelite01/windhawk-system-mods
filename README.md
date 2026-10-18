# windhawk-system-mods

Original [Windhawk](https://windhawk.net) mods that restyle Windows system
dialogs. Each mod is a single file in `mods/`.

All code in this repo is written from scratch and licensed MIT (see `LICENSE`).

## Mods

| Mod | File | Status |
|---|---|---|
| File Operation Glass | `mods/file-operation-glass.wh.cpp` | v0.1.0, untested draft |

### File Operation Glass
A restyled copy, move and delete dialog:

- dark window with an acrylic or Mica backdrop, rounded corners and an accent border
- a panel with a progress ring, live speed, a speed graph, time remaining,
  items remaining and the current file
- Pause and Cancel buttons that press the real buttons of the dialog
- themes (Glass, Midnight, Aurora, Light) and a custom accent color

Windows still performs the copying, so conflicts, errors and cancelling behave
as usual. Disabling the mod brings the original dialog back.

The idea of a restyled file operation window comes from
[File Operation Styler](https://github.com/digart11/File-Operation-Styler) by
digart11. This mod is a separate implementation: it reads progress through UI
Automation and draws its own panel.

## Install a mod locally
1. Open Windhawk and choose **Create a new mod**.
2. Paste the contents of the `.wh.cpp` file over the template.
3. Click **Compile Mod**, then enable the mod.
4. Test by starting a copy from Explorer with a folder that has many files.

## Diagnostic tools (`tools/`)
Used while working out how the file operation dialog is built. Not for the
catalog and not needed to use the mods.

| File | Purpose |
|---|---|
| `file-operation-probe.wh.cpp` | Logs the dialog's windows and UI Automation tree to `C:\Users\Public\file-operation-probe.log` |
| `hello-probe.wh.cpp` | Minimal mod that writes one line, to check that mods load |
| `run-copy-test.bat`, `copy-dialog.vbs` | Create test files and run a copy so the dialog appears |

## Submitting to the Windhawk catalog
Open a pull request to `ramensoftware/windhawk-mods` that adds only
`mods/<mod-id>.wh.cpp`. The `@github` line in the file must match the
username of the person opening the pull request.

## Known limitations
- Text parsing for speed and time assumes English number and size formats.
- Applies to new dialogs only.
- Tested on Windows 11 only.
