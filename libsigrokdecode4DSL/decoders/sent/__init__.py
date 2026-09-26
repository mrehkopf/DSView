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
SENT (Single Edge Nibble Transmission, SAE J2716) is a unidirectional
single-wire protocol used by automotive sensors. Data is encoded in the
time between falling edges, measured in clock ticks (typically 3 us):
a 56-tick sync pulse, a status nibble, 1-6 data nibbles and a CRC nibble,
each nibble taking 12-27 ticks, optionally followed by a pause pulse.

The tick time is re-calibrated from every sync pulse. The decoder checks
the fast channel CRC (recommended and legacy algorithm) and decodes the
slow channel carried in the status nibble: short serial messages and
enhanced serial messages (12-bit data / 8-bit ID and 16-bit data / 4-bit ID).
'''

from .pd import Decoder
