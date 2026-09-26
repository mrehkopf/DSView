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

# TODO: Decode HDR-DDR/TSP/TSL/BT payloads (currently only skipped).

import sigrokdecode as srd

'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <pdata>]

<ptype>:
 - 'START' / 'START REPEAT' / 'STOP' (<pdata>: None)
 - 'ADDRESS READ' / 'ADDRESS WRITE' (<pdata>: 7-bit address)
 - 'ACK' / 'NACK' (<pdata>: None)
 - 'DATA READ' / 'DATA WRITE' (<pdata>: data byte)
 - 'CCC' (<pdata>: CCC code)
 - 'DAA' (<pdata>: [pid, bcr, dcr, dynamic address])
 - 'HDR' (<pdata>: HDR mode number, from ENTHDRx)
'''

BROADCAST_ADDR = 0x7E
HOT_JOIN_ADDR = 0x02

CCC_ENTDAA = 0x07
CCC_RSTDAA = 0x06

# Code: name. Broadcast CCCs are < 0x80, direct CCCs >= 0x80.
ccc_names = {
    0x00: 'ENEC', 0x01: 'DISEC', 0x02: 'ENTAS0', 0x03: 'ENTAS1',
    0x04: 'ENTAS2', 0x05: 'ENTAS3', 0x06: 'RSTDAA', 0x07: 'ENTDAA',
    0x08: 'DEFTGTS', 0x09: 'SETMWL', 0x0A: 'SETMRL', 0x0B: 'ENTTM',
    0x0C: 'SETBUSCON', 0x12: 'ENDXFER',
    0x20: 'ENTHDR0', 0x21: 'ENTHDR1', 0x22: 'ENTHDR2', 0x23: 'ENTHDR3',
    0x24: 'ENTHDR4', 0x25: 'ENTHDR5', 0x26: 'ENTHDR6', 0x27: 'ENTHDR7',
    0x28: 'SETXTIME', 0x29: 'SETAASA', 0x2A: 'RSTACT', 0x2B: 'DEFGRPA',
    0x2C: 'RSTGRPA', 0x2D: 'MLANE',
    0x80: 'ENEC', 0x81: 'DISEC', 0x82: 'ENTAS0', 0x83: 'ENTAS1',
    0x84: 'ENTAS2', 0x85: 'ENTAS3', 0x86: 'RSTDAA', 0x87: 'SETDASA',
    0x88: 'SETNEWDA', 0x89: 'SETMWL', 0x8A: 'SETMRL', 0x8B: 'GETMWL',
    0x8C: 'GETMRL', 0x8D: 'GETPID', 0x8E: 'GETBCR', 0x8F: 'GETDCR',
    0x90: 'GETSTATUS', 0x91: 'GETACCCR', 0x92: 'ENDXFER', 0x93: 'SETBRGTGT',
    0x94: 'GETMXDS', 0x95: 'GETCAPS', 0x96: 'SETROUTE', 0x97: 'D2DXFER',
    0x98: 'SETXTIME', 0x99: 'GETXTIME', 0x9A: 'RSTACT', 0x9B: 'SETGRPA',
    0x9C: 'RSTGRPA', 0x9D: 'MLANE',
}

hdr_modes = {0: 'HDR-DDR', 1: 'HDR-TSP', 2: 'HDR-TSL', 3: 'HDR-BT'}

# ENEC/DISEC event bits.
event_bits = ((0x01, 'INT'), (0x02, 'CR'), (0x08, 'HJ'))

def ccc_name(code):
    if code in ccc_names:
        return ccc_names[code]
    if 0x61 <= code <= 0x7F or 0xE0 <= code <= 0xFE:
        return 'Vendor'
    return 'Reserved'

def odd_parity(value):
    # Bit that makes the total number of ones (value + bit) odd.
    return 1 ^ (bin(value).count('1') & 1)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'i3c'
    name = 'I3C'
    longname = 'MIPI Improved Inter-Integrated Circuit'
    desc = 'Two-wire, multi-drop serial bus with dynamic addressing (SDR).'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['i3c']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'scl', 'type': 8, 'name': 'SCL', 'desc': 'Serial clock line'},
        {'id': 'sda', 'type': 108, 'name': 'SDA', 'desc': 'Serial data line'},
    )
    options = (
        {'id': 'i2c_addrs', 'desc': 'Legacy I²C addresses (hex, comma-separated)',
            'default': ''},
    )
    annotations = (
        ('7', 'start', 'Start condition'),
        ('6', 'repeat-start', 'Repeat start condition'),
        ('1', 'stop', 'Stop condition'),
        ('5', 'ack', 'ACK'),
        ('0', 'nack', 'NACK'),
        ('208', 'bit', 'Data/address bit'),
        ('112', 'address-read', 'Address read'),
        ('111', 'address-write', 'Address write'),
        ('110', 'data-read', 'Data read'),
        ('109', 'data-write', 'Data write'),
        ('65', 't-bit', 'T-bit (parity / end of data)'),
        ('75', 'ccc', 'Common command code'),
        ('70', 'daa', 'Dynamic address assignment'),
        ('50', 'hdr', 'HDR mode'),
        ('6', 'request', 'IBI / hot-join request'),
        ('1000', 'warnings', 'Human-readable warnings'),
    )
    annotation_rows = (
        ('bits', 'Bits', (5,)),
        ('addr-data', 'Address/Data', (0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 12, 13)),
        ('commands', 'Commands', (11, 14)),
        ('warnings', 'Warnings', (15,)),
    )
    binary = (
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.bits = []
        self.need = 0
        self.phase = 'idle'
        self.bitwidth = 1
        self.first_after_start = False
        self.target = None
        self.rnw = 0
        self.is_i2c = False
        self.expect_ccc = False
        self.ccc = None
        self.ccc_ss = None
        self.seg_bytes = []
        self.seg_ss = self.seg_es = None
        self.daa = None
        self.hdr_mode = None
        self.dyn_addrs = {}
        self.i2c_addrs = set()

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        for tok in self.options['i2c_addrs'].replace(';', ',').split(','):
            tok = tok.strip()
            if tok:
                try:
                    self.i2c_addrs.add(int(tok, 16) & 0x7F)
                except ValueError:
                    pass

    def putg(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    def putp(self, ss, es, data):
        self.put(ss, es, self.out_python, data)

    def warn(self, ss, es, texts):
        self.putg(ss, es, 15, texts)

    def expect(self, phase, nbits):
        self.phase = phase
        self.need = nbits
        self.bits = []

    # Returns (value, ss, es) of the collected bits and emits the bit row.
    def take_bits(self, bits):
        n = len(bits)
        if n > 1:
            self.bitwidth = max(1, (bits[-1][1] - bits[0][1]) // (n - 1))
        value = 0
        for i, (b, ss) in enumerate(bits):
            value = (value << 1) | b
            es = bits[i + 1][1] if i + 1 < n else ss + self.bitwidth
            self.putg(ss, es, 5, ['%d' % b])
        return value, bits[0][1], bits[-1][1] + self.bitwidth

    # Summarize the bytes of one CCC segment (bytes between two
    # (repeated) START conditions) on the command row.
    def flush_segment(self):
        data = self.seg_bytes
        if self.ccc is None or not data:
            self.seg_bytes = []
            return
        ss, es = self.seg_ss, self.seg_es
        code = self.ccc
        name = ccc_name(code)
        text = None
        if name in ('ENEC', 'DISEC'):
            ev = [n for m, n in event_bits if data[0] & m]
            text = '%s: %s' % (name, ' '.join(ev) if ev else 'none')
        elif name in ('SETMWL', 'SETMRL', 'GETMWL', 'GETMRL') and len(data) >= 2:
            what = 'max write' if name.endswith('MWL') else 'max read'
            text = '%s: %s %d bytes' % (name, what, (data[0] << 8) | data[1])
            if len(data) >= 3:
                text += ', IBI payload %d' % data[2]
        elif name in ('SETDASA', 'SETNEWDA') and self.target is not None:
            new = data[0] >> 1
            text = '%s: 0x%02X -> 0x%02X' % (name, self.target, new)
            self.dyn_addrs.pop(self.target, None)
            self.dyn_addrs[new] = None
        elif name == 'GETPID' and len(data) >= 6:
            pid = int.from_bytes(bytes(data[:6]), 'big')
            text = 'PID 0x%012X (MIPI mfr 0x%04X)' % (pid, pid >> 33)
        elif name in ('GETBCR', 'GETDCR') and self.target is not None:
            text = '%s 0x%02X: 0x%02X' % (name[3:], self.target, data[0])
        elif name == 'GETSTATUS' and len(data) >= 2:
            st = (data[0] << 8) | data[1]
            text = 'Status 0x%04X: %d pending IBI%s' % (st, st & 0x0F,
                    ', protocol error' if st & 0x20 else '')
        if text is None:
            text = '%s: %s' % (name, ' '.join('%02X' % b for b in data))
        self.putg(ss, es, 11, [text, name])
        self.seg_bytes = []

    def add_seg_byte(self, ss, es, value):
        if not self.seg_bytes:
            self.seg_ss = ss
        self.seg_bytes.append(value)
        self.seg_es = es

    def handle_start(self):
        s = self.samplenum
        repeat = self.phase != 'idle'
        self.flush_segment()
        if self.phase == 'daa_id' or self.phase == 'daa_addr':
            self.warn(s, s, ['ENTDAA aborted', 'DAA abort'])
        cmd = 'START REPEAT' if repeat else 'START'
        self.putp(s, s, [cmd, None])
        if repeat:
            self.putg(s, s, 1, ['Start repeat', 'Sr'])
        else:
            self.putg(s, s, 0, ['Start', 'S'])
            self.ccc = None
            self.expect_ccc = False
        self.first_after_start = not repeat
        self.target = None
        self.expect('addr', 8)

    def handle_stop(self):
        s = self.samplenum
        self.flush_segment()
        if self.phase in ('daa_id', 'daa_addr'):
            self.warn(s, s, ['ENTDAA aborted', 'DAA abort'])
        self.putp(s, s, ['STOP', None])
        self.putg(s, s, 2, ['Stop', 'P'])
        self.ccc = None
        self.expect_ccc = False
        self.target = None
        self.phase = 'idle'
        self.bits = []

    def handle_addr(self, v, ss, es):
        addr, rnw = v >> 1, v & 1
        self.rnw = rnw
        self.target = addr
        # Direct CCC targets are I3C devices, even at a legacy static address.
        self.is_i2c = addr in self.i2c_addrs and self.ccc is None
        if addr != BROADCAST_ADDR:
            # 7E/W followed by Sr was only the arbitration header of a
            # private transfer, not the start of a CCC.
            self.expect_ccc = False
        cls, cmd = (6, 'ADDRESS READ') if rnw else (7, 'ADDRESS WRITE')
        self.putp(ss, es, [cmd, addr])
        rw = ['Read', 'Rd', 'R'] if rnw else ['Write', 'Wr', 'W']
        if addr == BROADCAST_ADDR:
            label = ['Broadcast', 'Bcast', '7E']
        elif self.is_i2c:
            label = ['I²C address: 0x%02X' % addr, 'I²C 0x%02X' % addr, '%02X' % addr]
        else:
            label = ['Address: 0x%02X' % addr, 'A: 0x%02X' % addr, '%02X' % addr]
        self.putg(ss, es - self.bitwidth, cls, label)
        self.putg(es - self.bitwidth, es, cls, rw)

        if addr == BROADCAST_ADDR:
            if not rnw:
                self.flush_segment()
                self.expect_ccc = True
                self.ccc = None
            elif self.ccc != CCC_ENTDAA:
                self.warn(ss, es, ['Broadcast read outside ENTDAA',
                                   '7E/R w/o ENTDAA'])
        elif self.first_after_start:
            if addr == HOT_JOIN_ADDR and not rnw:
                self.putg(ss, es, 14, ['Hot-join request', 'Hot-join', 'HJ'])
            elif rnw and not self.is_i2c:
                self.putg(ss, es, 14, ['IBI request / private read', 'IBI?'])
            elif not rnw and not self.is_i2c:
                self.putg(ss, es, 14, ['Controller role request / private write',
                                       'CRR?'])
        self.first_after_start = False
        self.expect('addr_ack', 1)

    def handle_ack(self, b, ss, es, next_phase):
        cmd = 'NACK' if b else 'ACK'
        self.putp(ss, es, [cmd, None])
        if b:
            self.putg(ss, es, 4, ['NACK', 'N'])
        else:
            self.putg(ss, es, 3, ['ACK', 'A'])
        if b:
            self.expect('ignore', 0)
        else:
            self.expect(*next_phase)

    def handle_addr_ack(self, b, ss, es):
        if self.target == BROADCAST_ADDR and self.rnw:
            if b:
                self.putg(ss, es, 11, ['ENTDAA: no more targets', 'DAA done'])
            self.daa = None
            self.handle_ack(b, ss, es, ('daa_id', 64))
        elif self.is_i2c:
            self.handle_ack(b, ss, es, ('i2c_data', 8))
        else:
            self.handle_ack(b, ss, es, ('data', 9))

    def handle_data_byte(self, d, ss, es):
        cls, cmd = (8, 'DATA READ') if self.rnw else (9, 'DATA WRITE')
        self.putp(ss, es, [cmd, d])
        self.put(ss, es, self.out_binary, [0 if self.rnw else 1, bytes([d])])

        if self.expect_ccc and not self.rnw:
            self.expect_ccc = False
            self.ccc = d
            self.ccc_ss = ss
            name = ccc_name(d)
            kind = 'direct' if d >= 0x80 else 'broadcast'
            self.putp(ss, es, ['CCC', d])
            self.putg(ss, es, 9, ['CCC: {$}', 'C: {$}', '{$}', d])
            self.putg(ss, es, 11, ['CCC %s (%s)' % (name, kind), name])
            if 0x20 <= d <= 0x27:
                self.hdr_mode = d - 0x20
            return

        if self.ccc is not None:
            self.add_seg_byte(ss, es, d)
        if self.rnw:
            self.putg(ss, es, 8, ['Data read: {$}', 'DR: {$}', '{$}', d])
        else:
            self.putg(ss, es, 9, ['Data write: {$}', 'DW: {$}', '{$}', d])

    def handle_data(self, v, ss, es):
        d, t = v >> 1, v & 1
        tss = es - self.bitwidth
        self.handle_data_byte(d, ss, tss)
        if self.rnw:
            if t:
                self.putg(tss, es, 10, ['T: more data', 'T=1', 'T'])
                self.expect('data', 9)
            else:
                self.putg(tss, es, 10, ['T: end of data', 'T=0', 'T'])
                self.expect('ignore', 0)
            return
        if t == odd_parity(d):
            self.putg(tss, es, 10, ['Parity OK', 'T', 'T'])
        else:
            self.putg(tss, es, 10, ['Parity error', 'PE', 'E'])
            self.warn(tss, es, ['T-bit parity error', 'Parity error', 'PE'])
        if self.hdr_mode is not None:
            self.putp(es, es, ['HDR', self.hdr_mode])
            self.expect('hdr', 0)
            self.hdr_ss = es
            self.hdr_falls = 0
        else:
            self.expect('data', 9)

    def handle_daa_id(self, v, ss, es):
        pid, bcr, dcr = v >> 16, (v >> 8) & 0xFF, v & 0xFF
        w = self.bitwidth
        pes = ss + 48 * w
        self.putg(ss, pes, 12, ['PID: 0x%012X' % pid, 'PID', 'P'])
        self.putg(pes, pes + 8 * w, 12, ['BCR: 0x%02X' % bcr, 'BCR', 'B'])
        self.putg(pes + 8 * w, es, 12, ['DCR: 0x%02X' % dcr, 'DCR', 'D'])
        self.daa = [pid, bcr, dcr, ss]
        self.expect('daa_addr', 8)

    def handle_daa_addr(self, v, ss, es):
        addr, par = v >> 1, v & 1
        tss = es - self.bitwidth
        self.putg(ss, tss, 7, ['Dynamic address: 0x%02X' % addr,
                               'DA: 0x%02X' % addr, '%02X' % addr])
        if par == odd_parity(addr):
            self.putg(tss, es, 10, ['Parity OK', 'P', 'P'])
        else:
            self.putg(tss, es, 10, ['Parity error', 'PE', 'E'])
            self.warn(tss, es, ['Dynamic address parity error', 'Parity error', 'PE'])
        self.daa_addr = addr
        self.expect('daa_ack', 1)

    def handle_daa_ack(self, b, ss, es):
        if self.daa is not None and not b:
            pid, bcr, dcr, dss = self.daa
            self.dyn_addrs[self.daa_addr] = pid
            self.putp(dss, es, ['DAA', [pid, bcr, dcr, self.daa_addr]])
            self.putg(dss, es, 11, ['ENTDAA: PID 0x%012X BCR 0x%02X DCR 0x%02X -> 0x%02X'
                    % (pid, bcr, dcr, self.daa_addr), 'DAA -> 0x%02X' % self.daa_addr])
        elif b:
            self.warn(ss, es, ['Target NACKed dynamic address', 'DA NACK'])
        self.daa = None
        self.handle_ack(b, ss, es, ('ignore', 0))

    def handle_i2c_data(self, v, ss, es):
        self.handle_data_byte(v, ss, es)
        self.expect('i2c_ack', 1)

    def got_bits(self):
        v, ss, es = self.take_bits(self.bits)
        p = self.phase
        if p == 'addr':
            self.handle_addr(v, ss, es)
        elif p == 'addr_ack':
            self.handle_addr_ack(v, ss, es)
        elif p == 'data':
            self.handle_data(v, ss, es)
        elif p == 'daa_id':
            self.handle_daa_id(v, ss, es)
        elif p == 'daa_addr':
            self.handle_daa_addr(v, ss, es)
        elif p == 'daa_ack':
            self.handle_daa_ack(v, ss, es)
        elif p == 'i2c_data':
            self.handle_i2c_data(v, ss, es)
        elif p == 'i2c_ack':
            self.handle_ack(v, ss, es, ('i2c_data', 8))

    # HDR traffic is not decoded. SDA toggles at most once per SCL phase in
    # every HDR mode, so several SDA falling edges within one SCL low phase
    # can only be the HDR Restart (2) or HDR Exit (4) pattern.
    def decode_hdr(self):
        mode = hdr_modes.get(self.hdr_mode, 'HDR%d' % self.hdr_mode)
        while True:
            self.wait([{1: 'f'}, {0: 'e'}])
            s = self.samplenum
            if self.matched & 0b01 and self.matched & 0b10 == 0:
                self.hdr_falls += 1
                if self.hdr_falls == 4:
                    self.putg(self.hdr_ss, s, 13, ['%s data (not decoded)' % mode,
                                                   mode, 'HDR'])
                    self.putg(s, s, 13, ['HDR exit', 'Exit'])
                    self.hdr_mode = None
                    self.ccc = None
                    self.expect('ignore', 0)
                    return
            else:
                if self.hdr_falls == 2:
                    self.putg(self.hdr_ss, s, 13, ['%s data (not decoded)' % mode,
                                                   mode, 'HDR'])
                    self.putg(s, s, 13, ['HDR restart', 'Restart'])
                    self.hdr_ss = s
                self.hdr_falls = 0

    def decode(self):
        while True:
            if self.phase == 'hdr':
                self.decode_hdr()
                continue
            if self.phase == 'idle':
                # START: SCL high, SDA falling.
                self.wait({0: 'h', 1: 'f'})
                self.handle_start()
                continue
            # a) bit sample: SCL rising, b) Sr, c) P.
            (scl, sda) = self.wait([{0: 'r'}, {0: 'h', 1: 'f'}, {0: 'h', 1: 'r'}])
            if self.matched & 0b001:
                if self.phase == 'ignore':
                    continue
                self.bits.append((sda, self.samplenum))
                if len(self.bits) == self.need:
                    self.got_bits()
            elif self.matched & 0b010:
                self.handle_start()
            elif self.matched & 0b100:
                self.handle_stop()
