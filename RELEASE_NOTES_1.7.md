# Vinder 1.7

## Vinder now has its own settings

- Vinder no longer shares its settings with GNOME Files. They are stored in Vinder's own location (`/io/github/paullinux_gn/Vinder/`), so changing a view, sort order, or thumbnail option in one app no longer changes the other.
- On the first start after updating, Vinder copies the settings you had changed, including all Interface settings, from the old shared location. This happens once. Your GNOME Files settings are not touched.
- The settings schemas were renamed to `io.github.paullinux_gn.Vinder.*`, so installing Vinder from source no longer replaces GNOME Files' own schemas.

## Build and packaging

- Releases are built by GitHub Actions: the Vinder, Devel, and stock profiles are compiled and tested, and the Flatpak bundle is built from the same source.
- The release bundle comes with a SHA256 checksum file and a build provenance attestation.
- Fixed the Flatpak build against the current GNOME 51 SDK by installing all dependencies and Vinder into `/app/lib`.
- Added a test for the settings migration.

## Other

- Vinder developer credits are now kept separate from the upstream Nautilus acknowledgements.
- Rewrote the README with current features, screenshots, and install instructions.
