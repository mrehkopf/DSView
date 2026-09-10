##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2026 Patrick Felixberger
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

# Texas Instruments TMP112 digital temperature sensor (datasheet SBOS344H).
#
# TMP112 uses a pointer register scheme: a WRITE transaction's first data
# byte selects one of four 16-bit registers (Temperature, Configuration,
# T_LOW, T_HIGH); any following data bytes in that same WRITE transaction
# are the MSB/LSB of the selected register's new value. A READ transaction
# has no pointer byte of its own -- it returns the 16-bit value of whatever
# register the pointer is currently set to (last one selected by a WRITE,
# or the Temperature register at power-up).

import sigrokdecode as srd

REG_TEMPERATURE = 0x00
REG_CONFIG = 0x01
REG_TLOW = 0x02
REG_THIGH = 0x03

reg_name = {
    REG_TEMPERATURE: 'Temperature',
    REG_CONFIG: 'Configuration',
    REG_TLOW: 'T_LOW',
    REG_THIGH: 'T_HIGH',
}

fault_queue = {
    0b00: 1,
    0b01: 2,
    0b10: 4,
    0b11: 6,
}

conversion_rate = {
    0b00: '0.25 Hz',
    0b01: '1 Hz',
    0b10: '4 Hz',
    0b11: '8 Hz',
}


def twos_complement(val, bits):
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val


class Decoder(srd.Decoder):
    api_version = 3
    id = 'tmp112'
    name = 'TMP112'
    longname = 'Texas Instruments TMP112'
    desc = 'Digital I2C temperature sensor with pointer-register addressing.'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['Sensor', 'IC']
    annotations = (
        ('temperature', 'Temperature'),
        ('temperature-verbose', 'Temperature (verbose)'),
        ('config', 'Configuration (verbose)'),
        ('config-short', 'Configuration'),
        ('reg-select', 'Register select'),
        ('warnings', 'Warnings'),
    )
    annotation_rows = (
        ('temperature', 'Temperature', (0, 1)),
        ('config', 'Configuration', (2, 3)),
        ('reg-select', 'Register select', (4,)),
        ('warnings', 'Warnings', (5,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        self.reg = REG_TEMPERATURE  # Pointer register defaults to 0x00 at power-up.
        self.extended_mode = False  # EM bit shadow, tracked from Config register traffic.
        self.rw = None
        self.wbytes = []
        self.rbytes = []

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putx(self, ss, es, data):
        self.put(ss, es, self.out_ann, data)

    def putb(self, data):
        self.put(self.ss_block, self.es_block, self.out_ann, data)

    def warn_upon_invalid_slave(self, ss, es, addr):
        # ADD0 pin selects the 7-bit address: GND=0x48, V+=0x49, SDA=0x4A, SCL=0x4B.
        # The upstream i2c decoder's 'address_format' option controls whether
        # ADDRESS WRITE/READ deliver the bare 7-bit address (shifted) or the
        # full byte with the R/W bit still packed in at bit 0 (unshifted,
        # the default) -- accept either encoding here.
        shifted = addr in range(0x48, 0x4B + 1)
        unshifted = addr in range(0x48 << 1, (0x4B << 1) + 2)
        if not (shifted or unshifted):
            s = 'Warning: I2C slave 0x%02x not a TMP112 compatible address.'
            self.putx(ss, es, [5, [s % addr]])

    def decode_temperature_value(self, raw16):
        if self.extended_mode:
            value = twos_complement(raw16 >> 3, 13)
        else:
            value = twos_complement(raw16 >> 4, 12)
        return value * 0.0625

    def output_temperature_reg(self, label, raw16):
        celsius = self.decode_temperature_value(raw16)
        fahrenheit = celsius * 9.0 / 5.0 + 32.0
        self.putb([0, ['%s: %.4f degC' % (label, celsius),
                        '%.4f degC' % celsius]])
        self.putb([1, ['%s: %.4f degC (%.4f degF, raw=0x%04x)' %
                        (label, celsius, fahrenheit, raw16)]])

    def output_config_reg(self, raw16, from_write):
        os_bit = (raw16 >> 15) & 1
        fq = (raw16 >> 11) & 0b11
        pol = (raw16 >> 10) & 1
        tm = (raw16 >> 9) & 1
        sd = (raw16 >> 8) & 1
        cr = (raw16 >> 6) & 0b11
        al = (raw16 >> 5) & 1
        em = raw16 & 1

        self.extended_mode = bool(em)

        s = 'OS = %d: %s\n' % (os_bit, 'one-shot armed/converting' if os_bit else 'idle')
        s += 'Fault queue: %d consecutive fault(s)\n' % fault_queue[fq]
        s += 'ALERT polarity: active-%s\n' % ('high' if pol else 'low')
        s += 'Thermostat mode: %s\n' % ('interrupt' if tm else 'comparator')
        s += 'SD = %d: %s\n' % (sd, 'shutdown' if sd else 'continuous conversion')
        s += 'Conversion rate: %s\n' % conversion_rate[cr]
        s += 'AL (alert, read-only) = %d\n' % al
        s += 'EM = %d: %s' % (em, 'extended 13-bit mode' if em else 'normal 12-bit mode')

        s2 = 'SD=%s, CR=%s, FQ=%d, POL=%s, TM=%s, EM=%s' % (
            'shutdown' if sd else 'continuous',
            conversion_rate[cr], fault_queue[fq],
            'high' if pol else 'low',
            'interrupt' if tm else 'comparator',
            '13bit' if em else '12bit')

        self.putb([2, [s]])
        self.putb([3, [s2]])

    def output_register_write(self, reg, raw16):
        if reg == REG_TEMPERATURE:
            self.putb([5, ['Warning: Temperature register is read-only!']])
            self.output_temperature_reg('Temperature (invalid write)', raw16)
        elif reg == REG_CONFIG:
            self.output_config_reg(raw16, True)
        elif reg == REG_TLOW:
            self.output_temperature_reg('T_LOW', raw16)
        elif reg == REG_THIGH:
            self.output_temperature_reg('T_HIGH', raw16)

    def output_register_read(self, reg, raw16):
        if reg == REG_CONFIG:
            self.output_config_reg(raw16, False)
        else:
            self.output_temperature_reg(reg_name.get(reg, 'Register 0x%02x' % reg), raw16)

    def decode(self, ss, es, data):
        cmd, databyte = data
        self.ss, self.es = ss, es

        if self.state == 'IDLE':
            if cmd != 'START':
                return
            self.state = 'ADDR'

        elif self.state == 'ADDR':
            if cmd == 'ADDRESS WRITE':
                self.warn_upon_invalid_slave(ss, es, databyte)
                self.rw = 'WRITE'
                self.wbytes = []
                self.state = 'WRITE POINTER'
            elif cmd == 'ADDRESS READ':
                self.warn_upon_invalid_slave(ss, es, databyte)
                self.rw = 'READ'
                self.rbytes = []
                self.ss_block = None
                self.state = 'READ DATA'

        elif self.state == 'WRITE POINTER':
            if cmd == 'DATA WRITE':
                self.reg = databyte & 0x03
                self.putx(ss, es, [4, ['Select register: %s (0x%02x)' %
                                        (reg_name.get(self.reg, '?'), self.reg)]])
                self.ss_block = None
                self.state = 'WRITE DATA'
            elif cmd in ('START REPEAT', 'STOP'):
                self.state = 'ADDR' if cmd == 'START REPEAT' else 'IDLE'

        elif self.state == 'WRITE DATA':
            if cmd == 'DATA WRITE':
                if self.ss_block is None:
                    self.ss_block = ss
                self.wbytes.append(databyte)
                self.es_block = es
                if len(self.wbytes) == 2:
                    raw16 = (self.wbytes[0] << 8) | self.wbytes[1]
                    self.output_register_write(self.reg, raw16)
                    self.wbytes = []
            elif cmd == 'START REPEAT':
                if len(self.wbytes) == 1:
                    self.putb([5, ['Warning: incomplete register write (1 byte)']])
                self.wbytes = []
                self.state = 'ADDR'
            elif cmd == 'STOP':
                if len(self.wbytes) == 1:
                    self.putb([5, ['Warning: incomplete register write (1 byte)']])
                self.wbytes = []
                self.state = 'IDLE'

        elif self.state == 'READ DATA':
            if cmd == 'DATA READ':
                if self.ss_block is None:
                    self.ss_block = ss
                self.rbytes.append(databyte)
                self.es_block = es
                if len(self.rbytes) == 2:
                    raw16 = (self.rbytes[0] << 8) | self.rbytes[1]
                    self.output_register_read(self.reg, raw16)
                    self.rbytes = []
            elif cmd == 'START REPEAT':
                if len(self.rbytes) == 1:
                    self.putb([5, ['Warning: incomplete register read (1 byte)']])
                self.rbytes = []
                self.state = 'ADDR'
            elif cmd == 'STOP':
                if len(self.rbytes) == 1:
                    self.putb([5, ['Warning: incomplete register read (1 byte)']])
                self.rbytes = []
                self.state = 'IDLE'
