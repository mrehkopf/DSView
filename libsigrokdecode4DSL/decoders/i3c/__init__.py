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
MIPI I3C (Improved Inter-Integrated Circuit) is a two-wire (SCL, SDA)
successor of I²C with push-pull data, in-band interrupts and dynamic
address assignment.

This decoder handles SDR mode: private transfers, broadcast and direct
Common Command Codes (CCC), ENTDAA dynamic address assignment, in-band
interrupt / hot-join requests and T-bit parity / end-of-data. HDR mode
traffic is recognized and skipped until the HDR exit pattern.

Legacy I²C targets on the same bus use an ACK instead of a T-bit after
each data byte; list their addresses in the 'Legacy I²C addresses' option
so their transfers are decoded correctly.
'''

from .pd import Decoder
