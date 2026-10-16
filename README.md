![DreamSourceLab Logo](DSView/icons/dsl_logo.svg)

# DSView

DSView is a GUI program for supporting various instruments from [DreamSourceLab](http://www.dreamsourcelab.com), including logic analyzers, oscilloscopes, etc. DSView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

This repository is a fork of the upstream
[DreamSourceLab/DSView](https://github.com/DreamSourceLab/DSView). It also
includes the Qt 6 port and the UI and driver improvements from
[Schildkroet/DSView](https://github.com/Schildkroet/DSView).

# What this fork adds

- **Logic analyzer stacking:** combine two DSLogic U3Pro32 analyzers into one
  capture with 64 channels (63 usable), with optional reference-based alignment and clock drift
  correction (see [Logic stacking](#logic-stacking)).
- **Parallel bus trigger entry:** set the trigger to a hexadecimal value of a
  configured Parallel decoder bus. See [doc/parallel-trigger.md](doc/parallel-trigger.md).
- **System theme:** use the Qt style configured by your desktop, including
  Kvantum (see [Appearance](#appearance)).
- Smaller fixes: no more busy-looping in the USB event thread (Windows), fewer redundant
  capture status repaints, reserved space for the waveform status and
  measurement bar.

# Status

The DSView software is in a usable state and has official tarball releases. However, it is still a work in progress. Some basic functionality is available and working, but other things are always on the TODO list.

# Download

Pre-built binaries of this fork (Linux .deb and AppImage, Windows installer and
zip) are built by CI from `master`. They are published as the
[latest pre-release](https://github.com/mrehkopf/DSView/releases/tag/latest).

Releases from [Schildkroet/DSView](https://github.com/Schildkroet/DSView/releases)
do not include the features listed above.

On Linux, the .deb package installs the udev rule that gives users access to
the hardware. When using the AppImage, install the rule manually:

```sh
sudo curl -L -o /etc/udev/rules.d/60-dreamsourcelab.rules \
    https://raw.githubusercontent.com/mrehkopf/DSView/master/DSView/DreamSourceLab.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

# Building

DSView uses CMake (≥ 3.10). It builds against Qt 6 by default and falls back
to Qt 5 if Qt 6 is not found. Required libraries: Qt (Core, Gui, Widgets, Svg),
glib ≥ 2.32, zlib, libusb-1.0 ≥ 1.0.16, Boost ≥ 1.42, FFTW 3 and Python 3
(development files).

The Qt Svg module is not optional, and CMake stops if it is missing. The themes
draw checkboxes, combobox arrows and window buttons from SVG files. If the qsvg
plugin is missing at runtime, those controls are not painted, and DSView only
logs a warning.

The [INSTALL](INSTALL) file has dependency lists for more distributions and
macOS.

## Linux

Debian/Ubuntu dependencies:

```sh
sudo apt install build-essential cmake git pkg-config \
    qt6-base-dev libqt6svg6-dev libgl1-mesa-dev libxkbcommon-dev libvulkan-dev \
    libglib2.0-dev zlib1g-dev libusb-1.0-0-dev libboost-dev libfftw3-dev \
    python3-dev libudev-dev
```

Fedora:

```sh
sudo dnf install gcc-c++ cmake git pkgconf qt6-qtbase-devel qt6-qtsvg-devel \
    glib2-devel zlib-devel libusb1-devel boost-devel fftw-devel \
    python3-devel systemd-devel
```

Build and install:

```sh
git clone https://github.com/mrehkopf/DSView.git
cd DSView
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Installing also copies the device firmware, the protocol decoders and the
translations. On Linux it copies the udev rule as well. To build a .deb package, run `cpack -G DEB` in the build directory.

## Windows (native, MSYS2)

This is the method used by CI. In a UCRT64 shell, install:

```sh
pacman -S git mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,qt6-base,qt6-svg,qt6-5compat,glib2,zlib,libusb,boost,fftw,python,pkg-config}
```

Then configure with MSYS2's own Python, so that the executable does not link
against some other Python installed on the system:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPython3_ROOT_DIR=/ucrt64 -DPython3_FIND_STRATEGY=LOCATION -DPython3_FIND_REGISTRY=NEVER
cmake --build build
```

See the `windows` job in [.github/workflows/build.yml](.github/workflows/build.yml)
for how the runtime DLLs are bundled.

## Windows (cross-compile from Linux, MXE)

[cross-compile-windows/dsview-build-mxe.sh](cross-compile-windows/dsview-build-mxe.sh)
builds a static 64-bit Qt 6 `DSView.exe` with [MXE](https://mxe.cc). It also
downloads the matching Python embeddable distribution, which has to be shipped
next to `DSView.exe`. Run it from the `cross-compile-windows` directory:

```sh
cd cross-compile-windows
MXE_HOME=~/src/mxe ./dsview-build-mxe.sh            # use an existing MXE checkout
./dsview-build-mxe.sh --auto-mxe                    # or clone MXE into $MXE_HOME
```

The first run builds the required MXE packages, which takes a long time. MXE
also needs Perl `Time::Piece` and Python `sphinx`, but does not check for them
in advance. The script header explains how to rebuild later without running
the whole script again, which files to deploy next to the executable, and how
to create an installer with `x86_64-w64-mingw32.static-cpack`. Run the script
with `QT_MAJOR=5` to build against Qt 5 instead, which a 32-bit build needs.

# Logic stacking

Stacking runs two **DSLogic U3Pro32** analyzers as a single logic analyzer
with 64 channels. Channels 0–31 come from the *master* and channels 32–63 from
the *secondary*.

## Setup

1. Connect the master's **trigger-out** port to one of the secondary's regular
   logic inputs. This input is the *sync input*. The secondary's dedicated
   trigger-in port can't be used yet, because the firmware doesn't support it.
2. Connect both analyzers via USB and select one of them in LOGIC mode.
3. Click **Stacking** in the sampling toolbar. In the dialog, enable
   two-analyzer stacking. Then select the **Master** and **Secondary** devices,
   and set **Secondary sync input** to the channel you wired in step 1.
4. Configure the trigger as usual. The trigger applies to the master, so only
   channels 0–31 can be used in it.

The secondary triggers on the rising edge that the master sends from its
trigger-out port. This lines up the two captures at the trigger point. The sync
input is used up by this, so the stacked capture has 63 usable channels.
The sync channel is hidden from the waveform view unless **Show secondary sync
channel** is checked.

**Secondary manual alignment** (in ns) adds a fixed time offset to the
secondary's data, for example to compensate for cable or probe delays.

Device options such as sample rate and depth are shared by both analyzers.
Both analyzers capture in buffer mode with all 32 channels enabled, so stream
mode is not available and the sample rate is limited to 250 MHz. Repeat mode,
loop mode and RLE compression are not available while stacking is enabled.
Instant captures (without a trigger) skip the trigger point alignment. They
apply the manual alignment, plus the reference corrections described below if
those are enabled and find matching edges.

Stacking settings are saved with the session. If a saved session is loaded with
different hardware, DSView asks you to remap the master and secondary in the
Stacking dialog.

## Reference alignment and drift correction

The sync trigger aligns the two captures at the trigger point. However, the two
analyzers run on independent clocks, so they can drift apart over a long
capture. Connect a shared reference signal, such as a clock or any signal with
regular edges, to one channel on each analyzer. Then select those channels
as **Master reference input** and **Secondary reference input**.

- **Align using reference signal** pairs edges of the reference signal on both
  analyzers and corrects the constant offset between them.
- **Correct clock drift using reference signal** also estimates the clock
  frequency difference (in ppm) and rescales the secondary's timeline to match.
- **Reference edge** selects which edges are used: rising, falling or both.

Edges are paired starting from the trigger point alignment. An edge on the
secondary only counts if it lies within about a quarter of the reference
signal's typical edge spacing. The reference signal therefore corrects small
remaining offsets, but not an offset of a whole reference period or more.

After each capture the status messages report the offset or drift that was
applied. A correction is skipped, and a message explains why, if:

- there are not enough matching edges,
- the edge offsets are too inconsistent, or
- the estimated drift is implausibly large.

# Appearance

Choose **Display → Themes → System** to use the Qt widget style and palette
configured by your desktop, including Kvantum. On Linux, restart DSView after
switching between System and a bundled theme to apply window decorations and
transparency. System mode uses native Linux window decorations and lets the
style manage window and dialog backgrounds. The waveform canvas keeps an opaque
background from the system palette for readability.

For Kvantum, install the plugin matching DSView's Qt version (Qt 6 for the default
build), and select it in your desktop's Qt style settings. You can also launch
with `QT_STYLE_OVERRIDE=kvantum DSView`. Translucency and blur depend on the active
Kvantum theme and compositor settings.

# Useful links

- [dreamsourcelab.com](https://www.dreamsourcelab.com)
- [kickstarter.com](https://www.kickstarter.com/projects/dreamsourcelab/dslogic-multifunction-instruments-for-everyone)
- [sigrok.org](https://sigrok.org)

# Copyright and license

DSView software is licensed under the terms of the GNU General Public License
(GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and
some files are licensed under the GPLv3+, this doesn't change the fact that
the program as a whole is licensed under the terms of the GPLv3+ (e.g. also
due to the fact that it links against GPLv3+ libraries).

Please see the individual source files for the full list of copyright holders.
