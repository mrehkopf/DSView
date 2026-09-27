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
## Convenience wrapper so a bare `make` in the source root works, while
## generated files stay in ./build. The executable goes to build.dir/.
##
##   make             configure ./build if needed, then build (in parallel)
##   make install     build if needed, then install (`sudo make install`)
##   make deb         build a .deb package in ./build
##   make clean       remove compiled objects, keep the configuration
##   make distclean   remove ./build entirely
##
##   make JOBS=8                      override the job count
##   make CMAKE_ARGS="-G Ninja"       extra arguments for the first configure
##
## Under sudo, configuring and compiling run as the invoking user ($SUDO_USER);
## only the copy into the system runs as root, and the install manifest it
## writes is handed back afterwards. So `sudo make install` never leaves
## root-owned files in ./build.
##

BUILD_DIR  ?= build
BUILD_TYPE ?= Release
CMAKE_ARGS ?=

# Leave two cores to the rest of the system, but never drop below one job.
# getconf is POSIX; sysctl covers macOS, and the trailing 1 covers both failing.
JOBS ?= $(shell \
	cores=$$(getconf _NPROCESSORS_ONLN 2>/dev/null \
		|| sysctl -n hw.ncpu 2>/dev/null \
		|| echo 1); \
	jobs=$$((cores - 2)); \
	[ "$$jobs" -ge 1 ] 2>/dev/null || jobs=1; \
	echo $$jobs)

# cmake drives its own (possibly make-based) build; don't leak our flags into it.
SUBCMAKE = env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS cmake

# Root via sudo: drop back to the calling user for everything but the install.
AS_USER :=
ifeq ($(shell id -u),0)
ifneq ($(filter-out root,$(SUDO_USER)),)
AS_USER := sudo -u $(SUDO_USER)
endif
endif

.PHONY: all build configure install deb clean distclean

all: build

$(BUILD_DIR)/CMakeCache.txt:
	$(AS_USER) $(SUBCMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_ARGS)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	$(AS_USER) $(SUBCMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

install: build
	$(SUBCMAKE) --install $(BUILD_DIR)
ifneq ($(AS_USER),)
	chown $(SUDO_UID):$(SUDO_GID) $(BUILD_DIR)/install_manifest*.txt
endif

deb: build
	cd $(BUILD_DIR) && $(AS_USER) cpack -G DEB

clean:
	@test ! -f $(BUILD_DIR)/CMakeCache.txt || $(AS_USER) $(SUBCMAKE) --build $(BUILD_DIR) --target clean

distclean:
	rm -rf $(BUILD_DIR)
