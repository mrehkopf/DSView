#!/bin/bash

# Utility script to cross-compile DSView for Windows using MXE
#
# Written by Maximilian Rehkopf <otakon@gmx.net>, (C) 2026
#
# Based on sigrok's sigrok-cross-mingw script by Uwe Hermann:
# https://sigrok.org/gitweb/?p=sigrok-util.git;a=blob;f=cross-compile/mingw/sigrok-cross-mingw
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.

# This script performs the following tasks:
# 1. Check for (and optionally download) MXE installation
# 2. Download Python source and embedded distribution
# 3. Build required MXE packages
# 4. Prepare Python development files for cross-compilation
# 5. Configure and build DSView using MXE toolchain

# Prerequisites:
# - MXE installed (may be downloaded automatically using --auto-mxe)
# - `wget` or `curl` for downloading files
# - `unzip` and `tar` for extracting files
# - `patch` utility for applying patches

# Additional undocumented MXE dependencies:
# MXE will not complain beforehand about these missing but compilation might
# fail without them.
#
# - Perl Time::Piece
#    * dnf install perl-Time-Piece
#    * apt install libtime-piece-perl (recently included with perl package)
#
# - Python 3 Module: Sphinx
#    * dnf install python3-sphinx
#    * apt install python3-sphinx

## Configuration variables - edit as needed
## Python version to use
PYTHON_VERSION="3.14.2"

## Target architecture: "i686" (32bit) or "x86_64" (64bit)
TARGET="x86_64"

## Path to MXE installation
MXE_HOME=${MXE_HOME:-$HOME/src/mxe}

## MXE target triplet
MXE_TARGET="${TARGET}-w64-mingw32.static"

# -----------------------------------------------------------------------------

die() {
    echo "$1"
    exit 1
}

# MXE packages to build
MXE_BUILD_PACKAGES="fftw libusb1 qt5 boost glib zlib gendef"

# Determine number of CPU cores for parallel builds
CORE_COUNT=$( nproc --all 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1 )

# Directories and URLs for Python setup
if [ $TARGET = "i686" ]; then
    PYTHON_TARGET="win32"
else
    PYTHON_TARGET="amd64"
fi
PYTHON_SETUP_DIR="python-setup"
PYTHON_DOWNLOAD_DIR="python-download"
PYTHON_URL_BASE="https://www.python.org/ftp/python/${PYTHON_VERSION}"
PYTHON_SRC_FILENAME="Python-${PYTHON_VERSION}.tar.xz"
PYTHON_EMBED_FILENAME="python-${PYTHON_VERSION}-embed-${PYTHON_TARGET}.zip"
PYTHON_SOURCE_URL="${PYTHON_URL_BASE}/${PYTHON_SRC_FILENAME}"
PYTHON_EMBED_URL="${PYTHON_URL_BASE}/${PYTHON_EMBED_FILENAME}"

WGET_CMD=$( command -v wget )
CURL_CMD=$( command -v curl )

[ a != "${WGET_CMD}"a ] && WGET_CMD="wget -N -q --show-progress -P ${PYTHON_DOWNLOAD_DIR}"
[ a != "${CURL_CMD}"a ] && CURL_CMD="curl -L -O --progress-bar --output-dir ${PYTHON_DOWNLOAD_DIR}"

DL_CMD=${WGET_CMD:-${CURL_CMD}}
[ -z "${DL_CMD}" ] && echo "Error: No download command found! Please install wget or curl." && exit 1

if [ ! -d "${MXE_HOME}" ]; then
    # MXE directory does not exist
    # clone MXE if --auto-mxe argument is provided
    if [ "$1" == "--auto-mxe" ]; then
        echo "MXE directory not found at: ${MXE_HOME}."
        echo "Cloning MXE repository into: ${MXE_HOME} ..."
        git clone https://github.com/mxe/mxe.git "${MXE_HOME}" || die "Error cloning MXE repository!"
    else
        echo "Error: MXE directory not found at: ${MXE_HOME}!"
        echo "Please adjust the MXE_HOME variable in this script to point to your MXE installation"
        echo "or set it externally before running this script."
        echo "Alternatively, run this script with --auto-mxe to download MXE automatically"
        echo "to the location specified above."
        exit 1
    fi
fi

mkdir -p "${PYTHON_SETUP_DIR}"

# Download Python source and embedded distribution
echo "Downloading Python ${PYTHON_VERSION} source..."
${DL_CMD} "${PYTHON_SOURCE_URL}" || die "Error downloading Python source from URL: ${PYTHON_SOURCE_URL}!";
echo "Downloading Python ${PYTHON_VERSION} embedded distribution..."
${DL_CMD} "${PYTHON_EMBED_URL}" || die "Error downloading Python embedded distribution from URL: ${PYTHON_EMBED_URL}!";

# Build MXE dependencies
for pkg in ${MXE_BUILD_PACKAGES}; do
    echo "Building MXE package: ${pkg} for target: ${MXE_TARGET}"
    make -j${CORE_COUNT} -C ${MXE_HOME} MXE_TARGETS=${MXE_TARGET} ${pkg} \
        || die "Error building MXE package: ${pkg}!"
done

echo "MXE packages have been built."

# extract the python packages
mkdir -p ${PYTHON_SETUP_DIR}/include
unzip -o ${PYTHON_DOWNLOAD_DIR}/${PYTHON_EMBED_FILENAME} \
        -d ${PYTHON_SETUP_DIR}/python-dist \
        || die "Failed to extract Python embedded distribution!"
echo "Python embedded distribution extracted."
tar xf ${PYTHON_DOWNLOAD_DIR}/${PYTHON_SRC_FILENAME} \
        -C ${PYTHON_SETUP_DIR}/include \
        Python-${PYTHON_VERSION}/Include \
        Python-${PYTHON_VERSION}/PC/pyconfig.h \
        --strip-components=2 \
        || die "Failed to extract header files from Python source package!"
echo "Python header files extracted."

# generate python.a from python.dll
mkdir -p ${PYTHON_SETUP_DIR}/libs

PYTHON_REL=${PYTHON_VERSION%.*}
PYTHON_LIB_BASE="python${PYTHON_REL//./}"

cp ${PYTHON_SETUP_DIR}/python-dist/${PYTHON_LIB_BASE}.dll .
$MXE_HOME/usr/$MXE_TARGET/bin/gendef ${PYTHON_LIB_BASE}.dll || die "Failed to generate .def file from ${PYTHON_LIB_BASE}.dll!"
$MXE_HOME/usr/bin/$MXE_TARGET-dlltool \
    --dllname ${PYTHON_LIB_BASE}.dll \
    --def ${PYTHON_LIB_BASE}.def \
    --output-lib ${PYTHON_SETUP_DIR}/libs/lib${PYTHON_LIB_BASE}.a || die "Failed to generate libpython.a!"
rm -f ${PYTHON_LIB_BASE}.dll
echo "Generated libpython.a from ${PYTHON_LIB_BASE}.dll."

# patch pyconfig.h
patch -d ${PYTHON_SETUP_DIR}/include -p0 < pyconfig.patch || die "Failed to patch pyconfig.h!"
echo "Patched pyconfig.h."

# generate python3.pc file for pkg-config
ABS_PYTHON_SETUP_DIR=$( realpath ${PYTHON_SETUP_DIR} )
mkdir -p ${PYTHON_SETUP_DIR}/pkgconfig
cat >${PYTHON_SETUP_DIR}/pkgconfig/python3.pc <<EOF
prefix=${ABS_PYTHON_SETUP_DIR}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/libs
includedir=\${prefix}/include
Name: Python
Description: Python library
Version: ${PYTHON_VERSION}
Libs: -l${ABS_PYTHON_SETUP_DIR}/libs/lib${PYTHON_LIB_BASE}.a
Cflags: -I${ABS_PYTHON_SETUP_DIR}/include
EOF
echo "Generated python3.pc for pkg-config."

if [ $TARGET = "i686" ]; then
    export PKG_CONFIG_PATH_i686_w64_mingw32_static="${ABS_PYTHON_SETUP_DIR}/pkgconfig"
else
    export PKG_CONFIG_PATH_x86_64_w64_mingw32_static="${ABS_PYTHON_SETUP_DIR}/pkgconfig"
fi

export PATH="${MXE_HOME}/usr/bin:${PATH}"
${MXE_HOME}/usr/bin/${MXE_TARGET}-pkg-config --cflags python3 || die "Error: pkg-config cannot find python3!"

${MXE_HOME}/usr/bin/${MXE_TARGET}-cmake ..
make -j${CORE_COUNT} || die "Error building DSView!"
echo "Build complete"
