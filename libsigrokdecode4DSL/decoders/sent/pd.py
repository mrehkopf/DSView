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

SYNC_TICKS = 56
CRC4_SEED = 0x5
CRC6_SEED = 0x15

class SamplerateError(Exception):
    pass

def crc_bits(bits, seed, width, poly):
    # Remainder of (seed * x^n + bits) mod poly, bits shifted in MSB-first.
    # poly excludes the x^width term.
    crc = seed
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    for b in bits:
        msb = crc & top
        crc = ((crc << 1) | b) & mask
        if msb:
            crc ^= poly
    return crc

def nibble_bits(nibbles):
    return [(n >> i) & 1 for n in nibbles for i in (3, 2, 1, 0)]

def crc4(nibbles, augment):
    # x^4 + x^3 + x^2 + 1, seed 0101. The J2010 recommended variant appends
    # a zero nibble, the legacy variant does not.
    bits = nibble_bits(nibbles) + ([0] * 4 if augment else [])
    return crc_bits(bits, CRC4_SEED, 4, 0xD)

def crc6(bits):
    # x^6 + x^4 + x^3 + 1, seed 010101, augmented with six zero bits.
    return crc_bits(bits + [0] * 6, CRC6_SEED, 6, 0x19)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'sent'
    name = 'SENT'
    longname = 'Single Edge Nibble Transmission (SAE J2716)'
    desc = 'Automotive single-wire sensor protocol.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Automotive']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'SENT data line'},
    )
    options = (
        {'id': 'tick', 'desc': 'Nominal tick time (us)', 'default': 3.0},
        {'id': 'nibbles', 'desc': 'Data nibbles per frame', 'default': 6,
            'values': (1, 2, 3, 4, 5, 6)},
        {'id': 'crc', 'desc': 'Fast channel CRC', 'default': 'recommended',
            'values': ('recommended', 'legacy')},
        {'id': 'format', 'desc': 'Fast channel format', 'default': 'hex',
            'values': ('hex', '12+12 bit', '16+8 bit')},
    )
    annotations = (
        ('sync', 'Sync/calibration pulse'),
        ('status', 'Status nibble'),
        ('data', 'Data nibble'),
        ('crc', 'CRC nibble'),
        ('pause', 'Pause pulse'),
        ('frame', 'Fast channel frame'),
        ('serial', 'Slow channel message'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('nibbles', 'Nibbles', (0, 1, 2, 3, 4)),
        ('frames', 'Fast channel', (5,)),
        ('serial', 'Slow channel', (6,)),
        ('warnings', 'Warnings', (7,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.tick = None
        self.slow = []
        self.pause_ok = False

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putg(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    def is_sync(self, samples, tick):
        return abs(samples / tick - SYNC_TICKS) <= SYNC_TICKS * 0.2

    def value_text(self, d):
        fmt = self.options['format']
        if fmt == '12+12 bit' and len(d) == 6:
            # J2716 H.1: signal 2 is transmitted least significant nibble first.
            a = (d[0] << 8) | (d[1] << 4) | d[2]
            b = (d[5] << 8) | (d[4] << 4) | d[3]
            return 'S1 %d (0x%03X), S2 %d (0x%03X)' % (a, a, b, b)
        if fmt == '16+8 bit' and len(d) == 6:
            a = (d[0] << 12) | (d[1] << 8) | (d[2] << 4) | d[3]
            b = (d[4] << 4) | d[5]
            return 'S1 %d (0x%04X), S2 %d (0x%02X)' % (a, a, b, b)
        return '0x' + ''.join('%X' % n for n in d)

    def handle_frame(self, sync_ss, pulses):
        status = pulses[0][0]
        data = [p[0] for p in pulses[1:-1]]
        crc = pulses[-1][0]
        rec, leg = crc4(data, True), crc4(data, False)
        want = rec if self.options['crc'] == 'recommended' else leg
        s_ss, s_es = pulses[0][1], pulses[0][2]
        self.putg(s_ss, s_es, 1, ['Status: 0x%X' % status, 'S:%X' % status, '%X' % status])
        for i, (n, ss, es) in enumerate(pulses[1:-1]):
            self.putg(ss, es, 2, ['D%d: 0x%X' % (i + 1, n), '%X' % n])
        c_ss, c_es = pulses[-1][1], pulses[-1][2]
        ok = crc == want
        if ok:
            self.putg(c_ss, c_es, 3, ['CRC OK: 0x%X' % crc, 'CRC', 'C'])
        else:
            other = leg if want == rec else rec
            note = ' (matches %s CRC)' % ('legacy' if want == rec else 'recommended') \
                if crc == other else ''
            self.putg(c_ss, c_es, 3, ['CRC error: 0x%X, expected 0x%X%s'
                                      % (crc, want, note), 'CRC!', 'E'])
            self.putg(c_ss, c_es, 7, ['Fast channel CRC error' + note, 'CRC error'])
        tick_us = self.tick / self.samplerate * 1e6
        t = self.value_text(data)
        self.putg(sync_ss, c_es, 5, ['%s  status 0x%X%s  (tick %.2f us)'
                                     % (t, status, '' if ok else '  CRC error', tick_us), t])
        self.slow_channel(status, sync_ss, c_es, ok)

    def slow_channel(self, status, ss, es, ok):
        if not ok:
            self.slow = []
            return
        self.slow.append(((status >> 3) & 1, (status >> 2) & 1, ss, es))
        del self.slow[:-18]
        f = self.slow
        # Enhanced serial message: bit 3 = 1111110 C xxxx 0 xxxx 0.
        if len(f) == 18 and all(x[0] for x in f[:6]) and not f[6][0] \
                and not f[12][0] and not f[17][0]:
            self.enhanced_message(f)
            self.slow = []
            return
        # Short serial message: bit 3 = 1 then 15 zeros.
        g = f[-16:]
        if len(g) == 16 and g[0][0] and not any(x[0] for x in g[1:]):
            self.short_message(g)
            self.slow = []

    def short_message(self, g):
        v = 0
        for x in g:
            v = (v << 1) | x[1]
        mid, data, crc = v >> 12, (v >> 4) & 0xFF, v & 0xF
        nib = [mid, data >> 4, data & 0xF]
        ok = crc in (crc4(nib, True), crc4(nib, False))
        ss, es = g[0][2], g[-1][3]
        self.putg(ss, es, 6, ['Short serial message: ID 0x%X data 0x%02X%s'
                              % (mid, data, '' if ok else ' CRC error'),
                              'SSM %X: %02X' % (mid, data)])
        if not ok:
            self.putg(ss, es, 7, ['Short serial message CRC error', 'SSM CRC'])

    def enhanced_message(self, f):
        b2 = [x[1] for x in f]
        b3 = [x[0] for x in f]
        crc = 0
        for b in b2[:6]:
            crc = (crc << 1) | b
        cfg = b3[7]
        hi = b3[8:12]
        lo = b3[13:17]
        data = 0
        for b in b2[6:]:
            data = (data << 1) | b
        bits = []
        for i in range(6, 18):
            bits += [b2[i], b3[i]]
        ok = crc6(bits) == crc
        if cfg:
            mid = int(''.join(map(str, hi)), 2)
            data |= int(''.join(map(str, lo)), 2) << 12
            t = 'ID 0x%X data 0x%04X (16-bit)' % (mid, data)
            short = 'ESM %X: %04X' % (mid, data)
        else:
            mid = int(''.join(map(str, hi + lo)), 2)
            t = 'ID 0x%02X data 0x%03X (12-bit)' % (mid, data)
            short = 'ESM %02X: %03X' % (mid, data)
        ss, es = f[0][2], f[-1][3]
        self.putg(ss, es, 6, ['Enhanced serial message: %s%s'
                              % (t, '' if ok else ' CRC error'), short])
        if not ok:
            self.putg(ss, es, 7, ['Enhanced serial message CRC error', 'ESM CRC'])

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        nominal = self.options['tick'] * 1e-6 * self.samplerate
        npulses = self.options['nibbles'] + 2
        self.wait({0: 'f'})
        last = self.samplenum
        sync_ss = None
        pulses = []
        while True:
            self.wait({0: 'f'})
            ss, es = last, self.samplenum
            d = es - ss
            last = es
            if self.is_sync(d, self.tick or nominal) or \
                    (self.tick and self.is_sync(d, nominal)):
                if sync_ss is not None and pulses:
                    self.putg(sync_ss, ss, 7, ['Incomplete frame', 'Short'])
                    self.slow = []
                self.tick = d / SYNC_TICKS
                self.putg(ss, es, 0, ['Sync (%.2f us/tick)' %
                                      (self.tick / self.samplerate * 1e6), 'Sync', 'S'])
                sync_ss, pulses = ss, []
                continue
            if sync_ss is None:
                # Between frames: an optional pause pulse, else noise.
                if self.pause_ok and 12 <= d / self.tick <= 768:
                    self.putg(ss, es, 4, ['Pause (%d ticks)' % round(d / self.tick),
                                          'Pause', 'P'])
                self.pause_ok = False
                continue
            ticks = d / self.tick
            n = int(round(ticks)) - 12
            if 0 <= n <= 15 and len(pulses) < npulses:
                pulses.append((n, ss, es))
                if len(pulses) == npulses:
                    self.handle_frame(sync_ss, pulses)
                    sync_ss = None
                    self.pause_ok = True
                continue
            self.putg(ss, es, 7, ['Invalid pulse: %.1f ticks' % ticks, 'Invalid'])
            sync_ss, pulses = None, []
            self.slow = []
