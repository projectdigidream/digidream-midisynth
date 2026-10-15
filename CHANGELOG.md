# Changelog

What changed in each release of DigidreamSynth, fixes first. Kept as fixes
land: anything not released yet is under **Unreleased**.

## Unreleased

### Fixed

- **An edit was not reported to the host** (the desktop app's edited dot, the
  studio's "changed" and its drafts) until the synth's audio status loop
  ticked, which it never does before audio has started: an edit made before
  the first click went unseen. The DAW now reports it at once.

## 0.2.0 -- 2026-10-14

The first public release. Rebuilt on 2026-10-15 (same version).

### Fixed

- The piano roll scrolled the wrong way, and only with two fingers.
- Undo and redo are within reach, and act on what you are editing.

### Changed

- `swan_lake.mid` left the test corpus and the tool.
- A README for users; the maintainer material is in DEVELOPING.md.
- MIT licence.
