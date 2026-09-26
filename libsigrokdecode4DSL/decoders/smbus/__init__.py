##
## This file is part of the libsigrokdecode project.
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
## along with this program; if not, see <http://www.gnu.org/licenses/>.
##

'''
SMBus (System Management Bus) and PMBus (Power Management Bus) are
command-based protocols on top of I²C. Each transaction starts with a
command byte, followed by byte, word (little-endian) or block data and an
optional CRC-8 packet error code (PEC).

This decoder stacks on top of the 'i2c' PD. It reconstructs the SMBus
transaction type (quick command, send/receive byte, read/write byte/word,
block read/write, process call, host notify, alert response), checks the
PEC and optionally decodes the PMBus or Smart Battery (SBS) command set,
including PMBus LINEAR11/LINEAR16 values and status registers.

Set 'I²C address format' to match the option of the i2c decoder below.
'''

from .pd import Decoder
