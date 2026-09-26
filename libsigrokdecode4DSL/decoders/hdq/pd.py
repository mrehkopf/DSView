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

import sigrokdecode as srd

# Low-time thresholds (us): '1' < T_BIT < '0' < T_BREAK < break.
T_BIT = 70
T_BREAK = 170
# Nominal bit cycle (us), used for the end of the last bit of a byte.
T_CYCLE = 190
# Max gap (us) between two bits of the same byte.
T_TIMEOUT = 1000

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
    api_version = 3
    id = 'hdq'
    name = 'HDQ'
    longname = 'TI HDQ single-wire interface'
    desc = 'Single-wire battery fuel gauge interface.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Embedded/industrial', 'IC']
    channels = (
        {'id': 'hdq', 'name': 'HDQ', 'desc': 'HDQ data line'},
    )
    options = (
        {'id': 'data_bits', 'desc': 'Data bits', 'default': 8, 'values': (8, 16)},
    )
    annotations = (
        ('bit', 'Bit'),
        ('break', 'Break'),
        ('command', 'Command'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
        ('word', 'Word'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0,)),
        ('fields', 'Fields', (1, 2, 3, 4)),
        ('words', 'Words', (5,)),
        ('warnings', 'Warnings', (6,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.state = 'CMD'
        self.bits = []
        self.addr = None
        self.write = False
        self.last_read = None

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putg(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    def us(self, t):
        return int(t * 1e-6 * self.samplerate)

    def take_byte(self):
        bits = self.bits
        v = 0
        for i, (b, ss) in enumerate(bits):
            v |= b << i
            es = bits[i + 1][1] if i + 1 < len(bits) else ss + self.us(T_CYCLE)
            self.putg(ss, es, 0, ['%d' % b])
        self.bits = []
        return v, bits[0][1], bits[-1][1] + self.us(T_CYCLE)

    def handle_cmd(self):
        v, ss, es = self.take_byte()
        self.addr, self.write = v & 0x7F, bool(v & 0x80)
        rw = 'Write' if self.write else 'Read'
        self.putg(ss, es, 2, ['%s register 0x%02X' % (rw, self.addr),
                              '%s 0x%02X' % (rw[0], self.addr), '%02X' % self.addr])
        self.state = 'DATA'

    def handle_data(self):
        v, ss, es = self.take_byte()
        w = self.options['data_bits'] == 16
        f = '0x%04X' if w else '0x%02X'
        if self.write:
            self.putg(ss, es, 4, [('Write data: ' + f) % v, f % v])
            self.last_read = None
        else:
            self.putg(ss, es, 3, [('Read data: ' + f) % v, f % v])
            lr = self.last_read
            if not w and lr and lr[0] % 2 == 0 and lr[0] + 1 == self.addr:
                word = lr[1] | (v << 8)
                self.putg(lr[2], es, 5, ['Word 0x%02X: 0x%04X (%d)' % (lr[0], word, word),
                                         '0x%04X' % word])
                self.last_read = None
            else:
                self.last_read = (self.addr, v, self.cmd_ss)
        self.state = 'CMD'

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        while True:
            self.wait({0: 'f'})
            ss = self.samplenum
            if self.bits and ss - self.bits[-1][1] > self.us(T_TIMEOUT):
                self.putg(self.bits[0][1], self.bits[-1][1] + self.us(T_CYCLE), 6,
                          ['Incomplete byte (%d bits)' % len(self.bits), 'Incomplete'])
                self.bits = []
                self.state = 'CMD'
            self.wait({0: 'r'})
            low = (self.samplenum - ss) / self.samplerate * 1e6
            if low > T_BREAK:
                self.putg(ss, self.samplenum, 1, ['Break', 'Brk', 'B'])
                if self.bits:
                    self.putg(self.bits[0][1], ss, 6, ['Byte interrupted by break',
                                                        'Interrupted'])
                self.bits = []
                self.state = 'CMD'
                self.last_read = None
                continue
            if not self.bits and self.state == 'CMD':
                self.cmd_ss = ss
            self.bits.append((1 if low < T_BIT else 0, ss))
            need = 8 if self.state == 'CMD' else self.options['data_bits']
            if len(self.bits) == need:
                if self.state == 'CMD':
                    self.handle_cmd()
                else:
                    self.handle_data()
