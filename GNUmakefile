##
## This file is part of the DSView project.
## DSView is based on PulseView.
##
## Copyright (C) 2026 Schildkroet
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
##

##
## Parallel-by-default wrapper around the CMake-generated Makefile.
##
## CMake emits a Makefile that builds serially unless -j is given. GNU make
## reads GNUmakefile in preference to Makefile, so a bare `make` lands here,
## which picks a job count and then includes the generated Makefile verbatim
## -- every target it defines still works exactly as before.
##
## A -j on the command line overrides the default chosen here.
##

# Leave two cores to the rest of the system, but never drop below one job.
# getconf is POSIX; sysctl covers macOS, and the trailing 1 covers both failing.
BUILD_JOBS := $(shell \
	cores=$$(getconf _NPROCESSORS_ONLN 2>/dev/null \
		|| sysctl -n hw.ncpu 2>/dev/null \
		|| echo 1); \
	jobs=$$((cores - 2)); \
	[ "$$jobs" -ge 1 ] 2>/dev/null || jobs=1; \
	echo $$jobs)

MAKEFLAGS += -j$(BUILD_JOBS)

ifeq (,$(wildcard $(CURDIR)/Makefile))
    $(error No generated Makefile in $(CURDIR). Configure the tree first, e.g. \
        `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` and build with \
        `cmake --build build -j$(BUILD_JOBS)`)
endif

include $(CURDIR)/Makefile
