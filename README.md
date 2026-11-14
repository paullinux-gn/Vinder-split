# Vinder

Vinder is a GNOME file manager based on [GNOME Files (Nautilus)](https://apps.gnome.org/Nautilus/). It adds an Interface dialog with theme and blur controls.

![Vinder showing a blurred sidebar and menu beside a light file grid](docs/vinder-screenshot.png)

## Appearance

Open the main menu and choose **Interface**. Select **Light**, **Dark**, or **Dark and Light** for a dark sidebar and light file area. Turn on **Blur** to give the sidebar and popup windows a translucent background, then adjust **Opacity (%)** and **Blur Strength**. **Blur Menus** controls menus and popovers separately while blur is enabled.

Desktop backdrop blur requires GNOME 51 or later.

## Build and run

Install Meson, Ninja, a C toolchain, the Blueprint compiler, and the Nautilus development dependencies for your distribution. The runtime uses Bubblewrap, LocalSearch, and xdg-user-dirs-gtk for file management features.

From the repository root, build and install into a private prefix:

```sh
meson setup build -Dprofile=Devel --prefix="$PWD/local"
meson compile -C build
meson install -C build
XDG_DATA_DIRS="$PWD/local/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" \
  "$PWD/local/bin/nautilus" --new-window
```

The development profile uses the separate application ID `org.gnome.Nautilus.Devel`.

## Source and licensing

Vinder builds on the work of the [GNOME Nautilus contributors](https://gitlab.gnome.org/GNOME/nautilus). See [LICENSE](LICENSE) for licensing terms. Report Vinder issues on [GitHub](https://github.com/paullinux-gn/vinder/issues).

## Flatpak

Download `Vinder-1.2-x86_64.flatpak` from the [GitHub release](https://github.com/paullinux-gn/vinder/releases/tag/v1.2). The bundle uses the GNOME 51 runtime from Flathub.

```sh
flatpak install --user Vinder-1.2-x86_64.flatpak
flatpak run io.github.paullinux_gn.Vinder
```

The Flatpak has its own application ID and can be installed alongside GNOME Files.
