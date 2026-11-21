# Vinder 1.6.1

A maintenance release. The file-manager features are the same as in 1.6; this release is about building and shipping Vinder reliably.

- Releases are now built by GitHub Actions: the Vinder, Devel, and stock profiles are compiled and tested, and the Flatpak bundle is built from the same source.
- The release bundle comes with a SHA256 checksum file and a build provenance attestation.
- Fixed the Flatpak build against the current GNOME 51 SDK: all dependencies and Vinder itself now install into `/app/lib`, so the build no longer breaks on `lib64` paths.
- Vinder developer credits are now kept separate from the upstream Nautilus acknowledgements.
- Rewrote the README with current features, screenshots, and install instructions.
