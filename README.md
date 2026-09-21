![DreamSourceLab Logo](DSView/icons/dsl_logo.svg)

# DSView

DSView is a GUI program for supporting various instruments from [DreamSourceLab](http://www.dreamsourcelab.com), including logic analyzers, oscilloscopes, etc. DSView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

# Status

The DSView software is in a usable state and has official tarball releases. However, it is still a work in progress. Some basic functionality is available and working, but other things are always on the TODO list.

# Download

Pre-built binaries are available on the [releases page](https://github.com/Schildkroet/DSView/releases).

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
