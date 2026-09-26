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
DShot is a digital protocol between flight controllers and brushless
motor ESCs. A frame has 16 bits: an 11-bit throttle/command value, a
telemetry request bit and a 4-bit CRC. Bits have a fixed period, a '1'
is high for ~75% and a '0' for ~37.5% of it. The bit rate
(DShot150/300/600/1200) is detected automatically.

With bidirectional DShot the signal is inverted, the CRC is inverted and
the ESC answers each frame on the same wire with a GCR-encoded eRPM or
extended telemetry (EDT) frame at 5/4 of the bit rate, which is decoded
as well.
'''

from .pd import Decoder
