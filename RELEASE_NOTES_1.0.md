# Vinder-split 1.0

Vinder-split 1.0 is the first release of the split-pane file manager, built on Vinder and GNOME Files (Nautilus).

## Highlights

- Browse two folders side by side, with an independent tab set in each pane.
- Switch between single-pane and duo-pane layouts from the headerbar or with **F3**.
- Focus the other pane with **F6** and open folders in the other pane from the sidebar and path menus.
- Move tabs between panes, reorder tabs within each pane, and preserve the split position across launches.
- Use the separate pane tab strips to select and close tabs.
- Keep Vinder-split settings separate from Vinder and GNOME Files.
- Retain Vinder's appearance controls, folder views, sidebar, and file-management features.

## Build from source

The release source can be built with Meson, Ninja, the Blueprint compiler, and the GTK 4/libadwaita development dependencies required by this checkout:

```sh
meson setup build -Dprofile=VinderSplit --prefix="$PWD/local"
meson compile -C build
meson install -C build
```

The application ID is `io.github.paullinux_gn.VinderSplit`.

The release page includes a GNOME 51 Flatpak bundle for x86_64 and its SHA256 checksum file.
