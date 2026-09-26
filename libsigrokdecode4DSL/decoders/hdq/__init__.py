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
HDQ is Texas Instruments' single-wire interface for battery fuel gauges
(bq27xxx, bq26xxx, bq2018, ...). The idle-high line is pulled low for
each bit: a short low time is a '1', a long one a '0'. A long low pulse
(break) resets the interface. Each transaction is an 8-bit command
(7-bit register address, bit 7 set for writes) sent LSB-first by the
host, followed by 8 (or 16 with HDQ16) data bits written by the host
or returned by the gauge.

Reads of an even register immediately followed by the next register are
combined into a 16-bit little-endian word, as used by the bq27xxx
standard commands.
'''

from .pd import Decoder
