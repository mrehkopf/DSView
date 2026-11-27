![DreamSourceLab Logo](DSView/icons/dsl_logo.svg)

# DSView

DSView is a GUI program for supporting various instruments from [DreamSourceLab](http://www.dreamsourcelab.com), including logic analyzers, oscilloscopes, etc. DSView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

This repository is a fork of the upstream
[DreamSourceLab/DSView](https://github.com/DreamSourceLab/DSView). It also
includes the Qt 6 port and the UI and driver improvements from
[Schildkroet/DSView](https://github.com/Schildkroet/DSView).

# What this fork adds

- **Logic analyzer stacking:** combine two or more DSLogic U3Pro32 analyzers
  (up to eight) into one capture with 32 channels per analyzer, with optional
  reference-based alignment and clock drift correction (see
  [Logic stacking](#logic-stacking)).
- **Parallel bus trigger entry:** set the trigger to a hexadecimal value of a
  configured Parallel decoder bus. See [doc/parallel-trigger.md](doc/parallel-trigger.md).
- **Search:** find events and states in the captured data with conditions on
  what happens before, after or during them: bus values in hex or binary with
  don't-care bits and edges, comparisons and ranges, pulse widths, absence of
  activity, counts, and the timing between events, with a sortable list of
  matches. See [doc/search.md](doc/search.md).
- **System theme:** use the Qt style configured by your desktop, including
  Kvantum (see [Appearance](#appearance)).
- **Time bar and navigation:** a thin bar above the time ruler stands for the
  whole capture and marks the part in view, the trigger and the cursors. Click
  or drag on it to move through the capture, right-drag to zoom to a span, and
  double-click for the whole capture and back. **Begin**, **Trig Pos** and
  **End** on the toolbar jump there without changing the zoom. `[` and `]`
  zoom, and `F` shows the whole capture. From
  [lucapandolfini/DSView](https://github.com/lucapandolfini/DSView) and
  [DreamSourceLab/DSView#908](https://github.com/DreamSourceLab/DSView/pull/908).
- **Identify:** blink a DSLogic's or DSCope's LED red and green for two
  seconds, to see which device is which. The button is in the **Options**
  dialog and, for each stacked analyzer, in the Stacking dialog.
- **Delete data before or after a cursor:** in logic mode, right-click a
  cursor's label on the ruler or its line over the waveform. DSView asks
  first, as this can't be undone. The trigger, the other cursors and the
  decoders' sample ranges move with the data, and cursors in the deleted part
  are removed. Without the trigger, times count from the first sample. The cut
  falls on a whole word of 64 samples. From
  [lucapandolfini/DSView](https://github.com/lucapandolfini/DSView).
- **Control interface:** scripts and AI agents can set up the analyzer and its
  trigger, run captures, decode protocols, look at the captured data (edges,
  levels, measurements, searches), export VCD or CSV, and save or open
  data and configuration files, through the `dsview-ctl` command line tool
  or JSON-RPC on a local socket. DSView can run without a window
  (`--headless`). See [doc/control-interface.md](doc/control-interface.md).
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

Stacking runs two or more **DSLogic U3Pro32** analyzers (up to eight) as a
single logic analyzer. Each analyzer contributes 32 channels: the *master* (A1)
has channels 0–31, the first *secondary* (A2) channels 32–63, A3 channels 64–95,
and so on. The channels are named `A1-D0`, `A2-D0`, ….

## Setup

1. Connect the master's **trigger-out** port to one of the regular logic inputs
   of each secondary. This input is that secondary's *sync input*. The
   secondaries' dedicated trigger-in ports can't be used yet, because the
   firmware doesn't support it.
2. Connect all analyzers via USB and select one of them in LOGIC mode.
3. Click **Stacking** in the sampling toolbar. In the dialog, enable stacking
   and set the **Number of analyzers**. Then select a device for each analyzer
   (A1 is the master) and set each secondary's **Sync input** to the channel you
   wired in step 1. **Threshold** and **Filter** are set per analyzer here as
   well. To see which analyzer a device is, click **Identify** next to it: that
   analyzer's LED blinks red and green for two seconds.
4. Configure the trigger as usual. The trigger applies to the master, so only
   channels 0–31 can be used in it. A parallel bus trigger on a bus that spans
   several analyzers matches only the master's bits (see
   [doc/parallel-trigger.md](doc/parallel-trigger.md#stacking)).

Each secondary triggers on the rising edge that the master sends from its
trigger-out port. This lines up every secondary's capture with the master's at
the trigger point. Each secondary's sync input is used up by this, so with 32
channels per analyzer, two stacked analyzers have 63 usable channels, three
have 94 and four have 125. The sync channels are hidden from the waveform view
unless **Show secondary sync channels** is checked.

**Manual alignment** (in ns) adds a fixed time offset to a secondary's data,
for example to compensate for cable or probe delays. It is set per secondary.

Sample rate, duration and the external clock options are shared by all
analyzers. All analyzers capture in buffer mode, so stream mode is not
available. Repeat mode, loop mode and RLE compression are not available while
stacking is enabled.

Instant captures (without a trigger) skip the trigger point alignment. They
apply the manual alignment, plus the reference corrections described below if
those are enabled and find matching edges.

Every analyzer's data is held in memory until all of them have finished
uploading and the captures are merged. Memory use therefore grows with the
number of analyzers, and during the merge it is about twice that of the merged
capture.

DSLogic analyzers have no serial number, so DSView tells them apart by the USB
port they are plugged into, shown in the device list as for example
`DSLogic U3Pro32 [USB 8-3.2.1]` (bus 8, then the hub ports down to the
analyzer). Keep each analyzer in the same USB port:

- An analyzer that is unplugged and plugged into the same port again takes its
  place in the stacking setup again.
- If the master is unplugged, DSView switches to the demo device. When the
  master is back in its port, DSView switches back to it with the stacking
  setup, after waiting up to 3 s for the other analyzers.
- Stacking settings are saved with the session, including each analyzer's
  port. If a session is loaded and an analyzer isn't found at its port (it was
  moved, or the session was saved by an older build that named devices by bus
  and address), DSView asks you to remap the analyzers in the Stacking dialog.

Where the system doesn't report the port (on macOS, DSView reads the port from
the device's location ID), the device list shows the bus and address instead,
for example `[USB 8.61]`. These change whenever an analyzer is plugged in
again, so analyzers are then not found again automatically.

## Channels

The **Options** dialog sets the channels for all stacked analyzers:

- **Channel mode:** every analyzer uses channels 0–31 (up to 250 MHz), 0–15
  (up to 500 MHz) or 0–7 (up to 1 GHz). A mode is unavailable while a sync or
  reference input lies outside its channels; its tooltip names the inputs to
  move in the Stacking dialog first.
- **Inputs:** each analyzer has its own row of channels to capture. Sync
  inputs, and reference inputs while they are used, are always captured.
- **Identify:** blinks that analyzer's LED, as in the Stacking dialog.

Channel numbers stay 32 apart per analyzer whatever the mode, so A2 always
starts at channel 32. Names and decoders survive a change of mode or inputs,
except on channels that disappear.

Fewer enabled channels leave more memory per channel. All analyzers record the
same number of samples, though, so the analyzer with the most enabled channels
limits the duration.

## Reference alignment and drift correction

The sync trigger aligns the captures at the trigger point. However, the
analyzers run on independent clocks, so they can drift apart over a long
capture. Connect a shared reference signal to one channel on each analyzer,
for example a clock or any other signal whose edges are at least about 100
samples apart. Then select those channels as each analyzer's **Reference
input**.

- **Align using reference signal** pairs edges of the reference signal on the
  master and a secondary and corrects the constant offset between them at the
  trigger point.
- **Correct clock drift using reference signal** also estimates the clock
  frequency difference (in ppm) and rescales the secondary's timeline to match.
- **Reference edge** selects which edges are used: rising, falling or both. If
  **Both** reports inconsistent edge timing, select a single edge instead.

Each secondary is aligned and corrected against the master on its own, so a
problem with one secondary's reference input only affects that analyzer.

The sync trigger lines the captures up only to within the delay between the
master's trigger-out and the secondary's trigger, which can be a few hundred
nanoseconds. Near the trigger, DSView therefore looks for the offset within
about 2 µs at which most reference edges line up. A reference that repeats
within that distance, such as a clock, lines up equally well at several
offsets; then the trigger alignment is kept, and the reference only corrects
offsets below about a quarter of its typical edge spacing. From there, edges are
paired outwards over the whole capture, following the drift as it builds up.

After each capture the status messages report how far each secondary was
corrected from the trigger alignment, and the drift that was applied. A correction is skipped, and a message explains why,
if:

- there are not enough matching edges,
- the paired edges don't follow a consistent timing (typically more than 2
  samples off), or
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
