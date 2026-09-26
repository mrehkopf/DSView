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

commands = {
    0: 'MOTOR_STOP', 1: 'BEEP1', 2: 'BEEP2', 3: 'BEEP3', 4: 'BEEP4', 5: 'BEEP5',
    6: 'ESC_INFO', 7: 'SPIN_DIRECTION_1', 8: 'SPIN_DIRECTION_2',
    9: '3D_MODE_OFF', 10: '3D_MODE_ON', 11: 'SETTINGS_REQUEST',
    12: 'SAVE_SETTINGS', 13: 'EXTENDED_TELEMETRY_ENABLE',
    14: 'EXTENDED_TELEMETRY_DISABLE', 20: 'SPIN_DIRECTION_NORMAL',
    21: 'SPIN_DIRECTION_REVERSED', 22: 'LED0_ON', 23: 'LED1_ON', 24: 'LED2_ON',
    25: 'LED3_ON', 26: 'LED0_OFF', 27: 'LED1_OFF', 28: 'LED2_OFF', 29: 'LED3_OFF',
    30: 'AUDIO_STREAM_MODE_TOGGLE', 31: 'SILENT_MODE_TOGGLE',
    32: 'SIGNAL_LINE_TELEMETRY_DISABLE', 33: 'SIGNAL_LINE_TELEMETRY_ENABLE',
    34: 'SIGNAL_LINE_CONTINUOUS_ERPM_TELEMETRY',
    35: 'SIGNAL_LINE_CONTINUOUS_ERPM_PERIOD_TELEMETRY',
    42: 'SIGNAL_LINE_TEMPERATURE_TELEMETRY', 43: 'SIGNAL_LINE_VOLTAGE_TELEMETRY',
    44: 'SIGNAL_LINE_CURRENT_TELEMETRY', 45: 'SIGNAL_LINE_CONSUMPTION_TELEMETRY',
    46: 'SIGNAL_LINE_ERPM_TELEMETRY', 47: 'SIGNAL_LINE_ERPM_PERIOD_TELEMETRY',
}

# GCR 5b -> 4b.
gcr_table = {0x19: 0x0, 0x1B: 0x1, 0x12: 0x2, 0x13: 0x3, 0x1D: 0x4, 0x15: 0x5,
             0x16: 0x6, 0x17: 0x7, 0x1A: 0x8, 0x09: 0x9, 0x0A: 0xA, 0x0B: 0xB,
             0x1E: 0xC, 0x0D: 0xD, 0x0E: 0xE, 0x0F: 0xF}

edt_types = {0x2: ('Temperature', '%d degC', 1), 0x4: ('Voltage', '%.2f V', 0.25),
             0x6: ('Current', '%d A', 1), 0x8: ('Debug 1', '%d', 1),
             0xA: ('Debug 2', '%d', 1), 0xC: ('Stress level', '%d', 1),
             0xE: ('Status', '0x%02X', 1)}

def crc4(v12):
    return (v12 ^ (v12 >> 4) ^ (v12 >> 8)) & 0xF

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
    api_version = 3
    id = 'dshot'
    name = 'DShot'
    longname = 'Digital Shot ESC protocol'
    desc = 'Flight controller to brushless ESC protocol, incl. bidirectional.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'DShot signal line'},
    )
    options = (
        {'id': 'bidir', 'desc': 'Bidirectional (inverted) DShot', 'default': 'no',
            'values': ('no', 'yes')},
        {'id': 'poles', 'desc': 'Motor poles (for RPM)', 'default': 14},
    )
    annotations = (
        ('bit', 'Bit'),
        ('value', 'Throttle/command'),
        ('telem', 'Telemetry request'),
        ('crc', 'CRC'),
        ('frame', 'Frame'),
        ('reply', 'ESC telemetry'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0,)),
        ('fields', 'Fields', (1, 2, 3)),
        ('frames', 'Frames', (4, 5)),
        ('warnings', 'Warnings', (6,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putg(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    # Collect a 16-bit frame starting at the current active edge. Returns
    # (bits, period) or None if the frame ends early.
    def read_frame(self, act, idle):
        bits = []
        rise = self.samplenum
        period = None
        while True:
            self.wait({0: idle})
            high = self.samplenum - rise
            # A '0' is active for 37.5% of a period, so the next bit must
            # start within 8/3 * high (+ margin) of this one.
            limit = int(high * 3) + 2 if period is None else int(period * 1.5) + 2
            if len(bits) == 15:
                p = period or int(high / 0.75)
                bits.append((1 if high > p / 2 else 0, rise, rise + p))
                return bits, p
            self.wait([{0: act}, {'skip': limit}])
            if not self.matched & 0b01:
                return None
            p = self.samplenum - rise
            bits.append((1 if high > p / 2 else 0, rise, self.samplenum))
            period = p if period is None else (period + p) // 2
            rise = self.samplenum

    def handle_frame(self, bits, bidir):
        v = 0
        for b in bits:
            v = (v << 1) | b[0]
        for b, ss, es in bits:
            self.putg(ss, es, 0, ['%d' % b])
        val, tel, crc = v >> 5, (v >> 4) & 1, v & 0xF
        want = crc4(v >> 4) ^ (0xF if bidir else 0)
        ss, es = bits[0][1], bits[-1][2]
        v_es, t_es = bits[10][2], bits[11][2]
        if val == 0:
            txt = ['Disarm / motor stop', 'Stop', '0']
        elif val < 48:
            name = commands.get(val, 'CMD_%d' % val)
            txt = ['Command %d: %s' % (val, name), name, '%d' % val]
        else:
            txt = ['Throttle %d (%.1f%%)' % (val - 48, (val - 48) / 19.99),
                   'T %d' % (val - 48), '%d' % val]
        self.putg(ss, v_es, 1, txt)
        self.putg(v_es, t_es, 2, ['Telemetry request' if tel else 'No telemetry',
                                  'Tlm' if tel else '-', 'T' if tel else '-'])
        ok = crc == want
        if ok:
            self.putg(t_es, es, 3, ['CRC OK: 0x%X' % crc, 'CRC', 'C'])
        else:
            self.putg(t_es, es, 3, ['CRC error: 0x%X, expected 0x%X' % (crc, want),
                                    'CRC!', 'E'])
            self.putg(t_es, es, 6, ['DShot CRC error', 'CRC error'])
        period_us = (es - ss) / 16 / self.samplerate * 1e6
        rate = int(round(1000 / period_us / 150.0)) * 150
        self.putg(ss, es, 4, ['DShot%d: %s%s%s' % (rate, txt[0], ', telemetry' if tel else '',
                              '' if ok else ', CRC error'), txt[1]])

    def handle_reply(self, edges, bitlen):
        # Every edge is a '1' followed by as many '0' as bit times until
        # the next edge (NRZI). 21 bits in total, the first being the start;
        # the last edge's bits are padded like Betaflight does.
        value, nbits = 0, 0
        for a, b in zip(edges, edges[1:]):
            n = max(1, int(round((b - a) / bitlen)))
            if nbits + n > 21:
                break
            value = (value << n) | (1 << (n - 1))
            nbits += n
        ss, es = edges[0], edges[0] + int(21 * bitlen)
        if nbits < 18:
            self.putg(ss, es, 6, ['Telemetry reply too short', 'Short reply'])
            return
        if nbits < 21:
            n = 21 - nbits
            value = (value << n) | (1 << (n - 1))
        gcr = value ^ (value >> 1)
        dec = 0
        for i in range(4):
            q = (gcr >> (5 * i)) & 0x1F
            if q not in gcr_table:
                self.putg(ss, es, 6, ['Invalid GCR code in telemetry', 'GCR error'])
                return
            dec |= gcr_table[q] << (4 * i)
        payload, crc = dec >> 4, dec & 0xF
        if crc4(payload) ^ 0xF != crc:
            self.putg(ss, es, 5, ['Telemetry 0x%03X, CRC error' % payload, 'Tlm CRC!'])
            self.putg(ss, es, 6, ['Telemetry CRC error', 'CRC error'])
            return
        if not payload & 0x100 and payload & 0xE00:
            name, fmt, scale = edt_types.get(payload >> 8, ('EDT 0x%X' % (payload >> 8), '%d', 1))
            t = (name + ': ' + fmt) % ((payload & 0xFF) * scale)
            self.putg(ss, es, 5, [t, name])
            return
        per = (payload & 0x1FF) << (payload >> 9)
        if payload == 0xFFF or per == 0:
            self.putg(ss, es, 5, ['eRPM: 0 (motor stopped)', 'eRPM 0', '0'])
            return
        erpm = 60e6 / per
        rpm = erpm / max(1, self.options['poles'] // 2)
        self.putg(ss, es, 5, ['eRPM %d, %d RPM (period %d us)' % (erpm, rpm, per),
                              '%d RPM' % rpm])

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        bidir = self.options['bidir'] == 'yes'
        act, idle = ('f', 'r') if bidir else ('r', 'f')
        while True:
            self.wait({0: act})
            ss = self.samplenum
            res = self.read_frame(act, idle)
            if res is None:
                self.putg(ss, self.samplenum, 6, ['Incomplete frame', 'Short'])
                continue
            bits, period = res
            self.handle_frame(bits, bidir)
            if not bidir:
                continue
            # The ESC answers ~30 us after the frame, at 5/4 of the bit rate.
            bitlen = period * 4 / 5
            window = int(60e-6 * self.samplerate)
            self.wait([{0: 'f'}, {'skip': window}])
            if not self.matched & 0b01:
                continue
            edges = [self.samplenum]
            while True:
                self.wait([{0: 'e'}, {'skip': int(bitlen * 4)}])
                if not self.matched & 0b01:
                    break
                edges.append(self.samplenum)
            self.handle_reply(edges, bitlen)
