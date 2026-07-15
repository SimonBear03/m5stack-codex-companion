# M5Stack Codex Companion Agent Guide

This repository owns the M5Stack Codex Companion firmware, Python bridge, and
native macOS companion controller. It is portable and can be opened on its own.

## Start Here

1. Read `README.md` for the current product scope, setup, and validation path.
2. Read `planning/current-state.md` for the current implementation state and
   next hardware-validation work.
3. Read the relevant file under `docs/` before changing the BLE protocol,
   distribution flow, Cardputer behavior, or Mac Codex bridge.
4. Run `git status --short --branch` before meaningful edits.

## Workspace Memory Bridge

When a containing Simon workspace provides `system/pkm_memory_bridge.md` and
`9_pkm/`:

- Follow the bridge after meaningful project work.
- Read `9_pkm/AGENTS.md` before writing to the vault.
- Keep project and PKM Git changes separate; report pending memory handoffs.

When opened independently, follow this repo guide only.

## Work Mode

- Use the current branch for small documentation, narrow bug-fix, and focused
  validation work.
- Use a short-lived branch for larger firmware behavior, protocol, desktop
  integration, packaging, or multi-file experimental changes.
- Inspect and preserve existing work when the checkout is dirty.

## Project Boundaries

- `src/main.cpp` owns the StickS3 and Cardputer ADV firmware behavior.
- `bridge/sticks3_bridge/` owns the Python Desktop observer and App Server
  bridge.
- `scripts/` owns the macOS bridge supervisor and companion build helpers.
- `macos/` owns the native menu bar companion.
- `docs/` owns protocol, distribution, hardware-reference, and bridge details.
- `planning/current-state.md` owns the concise current implementation handoff.

## Validation

- For Python bridge changes, run the relevant tests under `tests/`; use
  `.bridge-venv/bin/python -m unittest discover -s tests` when that environment
  is available.
- For firmware changes, run the relevant PlatformIO build for `sticks3` and/or
  `cardputer_adv` using the available repo-local or system PlatformIO command.
- For macOS companion changes, use `scripts/build-macos-companion` and the
  documented manual validation path when running on a Mac.
- For documentation-only changes, review the Markdown and run
  `git diff --check`.
- Report any hardware or platform validation that remains pending on the
  current machine.

## Git

- Check `git status --short --branch` before and after meaningful edits.
- Pull with `git pull --ff-only` when the checkout is clean and staying current
  matters.
- Keep commits focused, reviewed, and supported by the relevant validation.
- Push changes that are ready to share under the active branch policy.

## Secrets And Runtime

Keep credentials, local pairing data, runtime status, logs, virtual
environments, PlatformIO output, generated apps, caches, and machine-local
launch configuration in their documented ignored locations.

## Reporting

Report the target hardware or bridge mode, validation performed, Git state,
and any remaining physical-device or macOS-only checks.
