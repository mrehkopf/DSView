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

# Well-known SMBus addresses (7-bit).
ADDR_HOST = 0x08
ADDR_ALERT_RESPONSE = 0x0C
ADDR_DEFAULT_DEVICE = 0x61

# Data formats:
#  s: send byte (no data)     b: byte            w: word
#  l11: PMBus LINEAR11 word   l16: PMBus LINEAR16 word (uses VOUT_MODE)
#  vm: VOUT_MODE byte         sb/sw: STATUS_BYTE/STATUS_WORD
#  bl: block (hex)            str: block (ASCII)
#  u16/i16: unsigned/signed word with unit   dk: word in 0.1 K
#  bst: SBS BatteryStatus     date: SBS ManufactureDate
pmbus_cmds = {
    0x00: ('PAGE', 'b', ''), 0x01: ('OPERATION', 'b', ''),
    0x02: ('ON_OFF_CONFIG', 'b', ''), 0x03: ('CLEAR_FAULTS', 's', ''),
    0x04: ('PHASE', 'b', ''), 0x05: ('PAGE_PLUS_WRITE', 'bl', ''),
    0x06: ('PAGE_PLUS_READ', 'bl', ''), 0x07: ('ZONE_CONFIG', 'w', ''),
    0x08: ('ZONE_ACTIVE', 'w', ''),
    0x10: ('WRITE_PROTECT', 'b', ''), 0x11: ('STORE_DEFAULT_ALL', 's', ''),
    0x12: ('RESTORE_DEFAULT_ALL', 's', ''), 0x13: ('STORE_DEFAULT_CODE', 'b', ''),
    0x14: ('RESTORE_DEFAULT_CODE', 'b', ''), 0x15: ('STORE_USER_ALL', 's', ''),
    0x16: ('RESTORE_USER_ALL', 's', ''), 0x17: ('STORE_USER_CODE', 'b', ''),
    0x18: ('RESTORE_USER_CODE', 'b', ''), 0x19: ('CAPABILITY', 'b', ''),
    0x1A: ('QUERY', 'bl', ''), 0x1B: ('SMBALERT_MASK', 'w', ''),
    0x20: ('VOUT_MODE', 'vm', ''), 0x21: ('VOUT_COMMAND', 'l16', 'V'),
    0x22: ('VOUT_TRIM', 'l16', 'V'), 0x23: ('VOUT_CAL_OFFSET', 'l16', 'V'),
    0x24: ('VOUT_MAX', 'l16', 'V'), 0x25: ('VOUT_MARGIN_HIGH', 'l16', 'V'),
    0x26: ('VOUT_MARGIN_LOW', 'l16', 'V'),
    0x27: ('VOUT_TRANSITION_RATE', 'l11', 'mV/us'),
    0x28: ('VOUT_DROOP', 'l11', 'mV/A'), 0x29: ('VOUT_SCALE_LOOP', 'l11', ''),
    0x2A: ('VOUT_SCALE_MONITOR', 'l11', ''), 0x2B: ('VOUT_MIN', 'l16', 'V'),
    0x30: ('COEFFICIENTS', 'bl', ''), 0x31: ('POUT_MAX', 'l11', 'W'),
    0x32: ('MAX_DUTY', 'l11', '%'), 0x33: ('FREQUENCY_SWITCH', 'l11', 'kHz'),
    0x34: ('POWER_MODE', 'b', ''), 0x35: ('VIN_ON', 'l11', 'V'),
    0x36: ('VIN_OFF', 'l11', 'V'), 0x37: ('INTERLEAVE', 'w', ''),
    0x38: ('IOUT_CAL_GAIN', 'l11', 'mOhm'), 0x39: ('IOUT_CAL_OFFSET', 'l11', 'A'),
    0x3A: ('FAN_CONFIG_1_2', 'b', ''), 0x3B: ('FAN_COMMAND_1', 'l11', ''),
    0x3C: ('FAN_COMMAND_2', 'l11', ''), 0x3D: ('FAN_CONFIG_3_4', 'b', ''),
    0x3E: ('FAN_COMMAND_3', 'l11', ''), 0x3F: ('FAN_COMMAND_4', 'l11', ''),
    0x40: ('VOUT_OV_FAULT_LIMIT', 'l16', 'V'), 0x41: ('VOUT_OV_FAULT_RESPONSE', 'b', ''),
    0x42: ('VOUT_OV_WARN_LIMIT', 'l16', 'V'), 0x43: ('VOUT_UV_WARN_LIMIT', 'l16', 'V'),
    0x44: ('VOUT_UV_FAULT_LIMIT', 'l16', 'V'), 0x45: ('VOUT_UV_FAULT_RESPONSE', 'b', ''),
    0x46: ('IOUT_OC_FAULT_LIMIT', 'l11', 'A'), 0x47: ('IOUT_OC_FAULT_RESPONSE', 'b', ''),
    0x48: ('IOUT_OC_LV_FAULT_LIMIT', 'l16', 'V'),
    0x49: ('IOUT_OC_LV_FAULT_RESPONSE', 'b', ''),
    0x4A: ('IOUT_OC_WARN_LIMIT', 'l11', 'A'), 0x4B: ('IOUT_UC_FAULT_LIMIT', 'l11', 'A'),
    0x4C: ('IOUT_UC_FAULT_RESPONSE', 'b', ''),
    0x4F: ('OT_FAULT_LIMIT', 'l11', 'degC'), 0x50: ('OT_FAULT_RESPONSE', 'b', ''),
    0x51: ('OT_WARN_LIMIT', 'l11', 'degC'), 0x52: ('UT_WARN_LIMIT', 'l11', 'degC'),
    0x53: ('UT_FAULT_LIMIT', 'l11', 'degC'), 0x54: ('UT_FAULT_RESPONSE', 'b', ''),
    0x55: ('VIN_OV_FAULT_LIMIT', 'l11', 'V'), 0x56: ('VIN_OV_FAULT_RESPONSE', 'b', ''),
    0x57: ('VIN_OV_WARN_LIMIT', 'l11', 'V'), 0x58: ('VIN_UV_WARN_LIMIT', 'l11', 'V'),
    0x59: ('VIN_UV_FAULT_LIMIT', 'l11', 'V'), 0x5A: ('VIN_UV_FAULT_RESPONSE', 'b', ''),
    0x5B: ('IIN_OC_FAULT_LIMIT', 'l11', 'A'), 0x5C: ('IIN_OC_FAULT_RESPONSE', 'b', ''),
    0x5D: ('IIN_OC_WARN_LIMIT', 'l11', 'A'), 0x5E: ('POWER_GOOD_ON', 'l16', 'V'),
    0x5F: ('POWER_GOOD_OFF', 'l16', 'V'), 0x60: ('TON_DELAY', 'l11', 'ms'),
    0x61: ('TON_RISE', 'l11', 'ms'), 0x62: ('TON_MAX_FAULT_LIMIT', 'l11', 'ms'),
    0x63: ('TON_MAX_FAULT_RESPONSE', 'b', ''), 0x64: ('TOFF_DELAY', 'l11', 'ms'),
    0x65: ('TOFF_FALL', 'l11', 'ms'), 0x66: ('TOFF_MAX_WARN_LIMIT', 'l11', 'ms'),
    0x68: ('POUT_OP_FAULT_LIMIT', 'l11', 'W'), 0x69: ('POUT_OP_FAULT_RESPONSE', 'b', ''),
    0x6A: ('POUT_OP_WARN_LIMIT', 'l11', 'W'), 0x6B: ('PIN_OP_WARN_LIMIT', 'l11', 'W'),
    0x78: ('STATUS_BYTE', 'sb', ''), 0x79: ('STATUS_WORD', 'sw', ''),
    0x7A: ('STATUS_VOUT', 'b', ''), 0x7B: ('STATUS_IOUT', 'b', ''),
    0x7C: ('STATUS_INPUT', 'b', ''), 0x7D: ('STATUS_TEMPERATURE', 'b', ''),
    0x7E: ('STATUS_CML', 'b', ''), 0x7F: ('STATUS_OTHER', 'b', ''),
    0x80: ('STATUS_MFR_SPECIFIC', 'b', ''), 0x81: ('STATUS_FANS_1_2', 'b', ''),
    0x82: ('STATUS_FANS_3_4', 'b', ''),
    0x86: ('READ_EIN', 'bl', ''), 0x87: ('READ_EOUT', 'bl', ''),
    0x88: ('READ_VIN', 'l11', 'V'), 0x89: ('READ_IIN', 'l11', 'A'),
    0x8A: ('READ_VCAP', 'l11', 'V'), 0x8B: ('READ_VOUT', 'l16', 'V'),
    0x8C: ('READ_IOUT', 'l11', 'A'), 0x8D: ('READ_TEMPERATURE_1', 'l11', 'degC'),
    0x8E: ('READ_TEMPERATURE_2', 'l11', 'degC'),
    0x8F: ('READ_TEMPERATURE_3', 'l11', 'degC'),
    0x90: ('READ_FAN_SPEED_1', 'l11', 'RPM'), 0x91: ('READ_FAN_SPEED_2', 'l11', 'RPM'),
    0x92: ('READ_FAN_SPEED_3', 'l11', 'RPM'), 0x93: ('READ_FAN_SPEED_4', 'l11', 'RPM'),
    0x94: ('READ_DUTY_CYCLE', 'l11', '%'), 0x95: ('READ_FREQUENCY', 'l11', 'kHz'),
    0x96: ('READ_POUT', 'l11', 'W'), 0x97: ('READ_PIN', 'l11', 'W'),
    0x98: ('PMBUS_REVISION', 'b', ''), 0x99: ('MFR_ID', 'str', ''),
    0x9A: ('MFR_MODEL', 'str', ''), 0x9B: ('MFR_REVISION', 'str', ''),
    0x9C: ('MFR_LOCATION', 'str', ''), 0x9D: ('MFR_DATE', 'str', ''),
    0x9E: ('MFR_SERIAL', 'str', ''), 0x9F: ('APP_PROFILE_SUPPORT', 'bl', ''),
    0xA0: ('MFR_VIN_MIN', 'l11', 'V'), 0xA1: ('MFR_VIN_MAX', 'l11', 'V'),
    0xA2: ('MFR_IIN_MAX', 'l11', 'A'), 0xA3: ('MFR_PIN_MAX', 'l11', 'W'),
    0xA4: ('MFR_VOUT_MIN', 'l16', 'V'), 0xA5: ('MFR_VOUT_MAX', 'l16', 'V'),
    0xA6: ('MFR_IOUT_MAX', 'l11', 'A'), 0xA7: ('MFR_POUT_MAX', 'l11', 'W'),
    0xA8: ('MFR_TAMBIENT_MAX', 'l11', 'degC'), 0xA9: ('MFR_TAMBIENT_MIN', 'l11', 'degC'),
    0xAA: ('MFR_EFFICIENCY_LL', 'bl', ''), 0xAB: ('MFR_EFFICIENCY_HL', 'bl', ''),
    0xAC: ('MFR_PIN_ACCURACY', 'b', ''), 0xAD: ('IC_DEVICE_ID', 'bl', ''),
    0xAE: ('IC_DEVICE_REV', 'bl', ''),
    0xC0: ('MFR_MAX_TEMP_1', 'l11', 'degC'), 0xC1: ('MFR_MAX_TEMP_2', 'l11', 'degC'),
    0xC2: ('MFR_MAX_TEMP_3', 'l11', 'degC'),
}
for _i in range(16):
    pmbus_cmds[0xB0 + _i] = ('USER_DATA_%02d' % _i, 'bl', '')

sbs_cmds = {
    0x00: ('ManufacturerAccess', 'w', ''), 0x01: ('RemainingCapacityAlarm', 'u16', 'mAh'),
    0x02: ('RemainingTimeAlarm', 'u16', 'min'), 0x03: ('BatteryMode', 'w', ''),
    0x04: ('AtRate', 'i16', 'mA'), 0x05: ('AtRateTimeToFull', 'u16', 'min'),
    0x06: ('AtRateTimeToEmpty', 'u16', 'min'), 0x07: ('AtRateOK', 'u16', ''),
    0x08: ('Temperature', 'dk', ''), 0x09: ('Voltage', 'u16', 'mV'),
    0x0A: ('Current', 'i16', 'mA'), 0x0B: ('AverageCurrent', 'i16', 'mA'),
    0x0C: ('MaxError', 'u16', '%'), 0x0D: ('RelativeStateOfCharge', 'u16', '%'),
    0x0E: ('AbsoluteStateOfCharge', 'u16', '%'), 0x0F: ('RemainingCapacity', 'u16', 'mAh'),
    0x10: ('FullChargeCapacity', 'u16', 'mAh'), 0x11: ('RunTimeToEmpty', 'u16', 'min'),
    0x12: ('AverageTimeToEmpty', 'u16', 'min'), 0x13: ('AverageTimeToFull', 'u16', 'min'),
    0x14: ('ChargingCurrent', 'u16', 'mA'), 0x15: ('ChargingVoltage', 'u16', 'mV'),
    0x16: ('BatteryStatus', 'bst', ''), 0x17: ('CycleCount', 'u16', ''),
    0x18: ('DesignCapacity', 'u16', 'mAh'), 0x19: ('DesignVoltage', 'u16', 'mV'),
    0x1A: ('SpecificationInfo', 'w', ''), 0x1B: ('ManufactureDate', 'date', ''),
    0x1C: ('SerialNumber', 'w', ''), 0x20: ('ManufacturerName', 'str', ''),
    0x21: ('DeviceName', 'str', ''), 0x22: ('DeviceChemistry', 'str', ''),
    0x23: ('ManufacturerData', 'bl', ''),
    0x3C: ('CellVoltage4', 'u16', 'mV'), 0x3D: ('CellVoltage3', 'u16', 'mV'),
    0x3E: ('CellVoltage2', 'u16', 'mV'), 0x3F: ('CellVoltage1', 'u16', 'mV'),
}

status_byte_bits = ('NONE_OF_THE_ABOVE', 'CML', 'TEMPERATURE', 'VIN_UV',
                    'IOUT_OC', 'VOUT_OV', 'OFF', 'BUSY')
status_word_bits = ('UNKNOWN', 'OTHER', 'FANS', 'POWER_GOOD#',
                    'MFR', 'INPUT', 'IOUT/POUT', 'VOUT')
battery_status_bits = {15: 'OCA', 14: 'TCA', 12: 'OTA', 11: 'TDA', 9: 'RCA',
                       8: 'RTA', 7: 'INIT', 6: 'DSG', 5: 'FC', 4: 'FD'}

data_len = {'s': 0, 'b': 1, 'vm': 1, 'sb': 1}

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def signed(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

def bitnames(v, names):
    return [n for i, n in enumerate(names) if v & (1 << i)]

def fmtnum(v, unit):
    t = '%d' % v if float(v).is_integer() else '%.4g' % v
    return ('%s %s' % (t, unit)).strip()

class Decoder(srd.Decoder):
    api_version = 3
    id = 'smbus'
    name = 'SMBus/PMBus'
    longname = 'System/Power Management Bus'
    desc = 'SMBus transactions with PEC, PMBus and Smart Battery commands.'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['Embedded/industrial', 'PC']
    options = (
        {'id': 'cmdset', 'desc': 'Command set', 'default': 'PMBus',
            'values': ('PMBus', 'Smart Battery', 'none')},
        {'id': 'pec', 'desc': 'Packet error code (PEC)', 'default': 'auto',
            'values': ('auto', 'yes', 'no')},
        {'id': 'addr_format', 'desc': 'I²C address format (as set in i2c)',
            'default': 'unshifted', 'values': ('unshifted', 'shifted')},
    )
    annotations = (
        ('command', 'Command'),
        ('count', 'Byte count'),
        ('data', 'Data'),
        ('pec', 'PEC'),
        ('transaction', 'Transaction'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('fields', 'Fields', (0, 1, 2, 3)),
        ('transactions', 'Transactions', (4,)),
        ('warnings', 'Warnings', (5,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.segs = []
        self.ss = None
        self.page = {}
        self.vout_exp = {}

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        cs = self.options['cmdset']
        self.cmds = pmbus_cmds if cs == 'PMBus' else sbs_cmds if cs == 'Smart Battery' else {}

    def putg(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    def decode(self, ss, es, data):
        cmd, d = data
        if cmd == 'START':
            self.segs = []
            self.ss = ss
        elif cmd == 'START REPEAT':
            if self.ss is None:
                self.ss = ss
        elif cmd in ('ADDRESS READ', 'ADDRESS WRITE'):
            if self.ss is None:
                return
            rw = 1 if cmd == 'ADDRESS READ' else 0
            addr = d >> 1 if self.options['addr_format'] == 'unshifted' else d
            self.segs.append({'addr': addr & 0x7F, 'rw': rw, 'data': [],
                              'ss': ss, 'es': es, 'nack': False})
        elif cmd in ('DATA READ', 'DATA WRITE'):
            if self.segs:
                self.segs[-1]['data'].append((d, ss, es))
        elif cmd == 'NACK':
            if self.segs and not self.segs[-1]['data']:
                self.segs[-1]['nack'] = True
        elif cmd == 'STOP':
            if self.ss is not None and self.segs:
                self.transaction(self.ss, es)
            self.segs = []
            self.ss = None

    # Split an optional trailing PEC byte off 'payload'. 'expected' is the
    # payload length without PEC, if known. Returns (payload, pec_ok).
    def split_pec(self, payload, expected):
        mode = self.options['pec']
        if mode == 'no' or not payload:
            return payload, None
        stream = []
        for s in self.segs:
            stream.append((s['addr'] << 1) | s['rw'])
            stream += [b[0] for b in s['data']]
        ok = crc8(stream[:-1]) == stream[-1]
        if expected is not None and mode == 'auto':
            if len(payload) == expected + 1:
                return payload[:-1], ok
            return payload, None
        if mode == 'auto' and not ok:
            return payload, None
        return payload[:-1], ok

    def expected_len(self, fmt, payload):
        if fmt in data_len:
            return data_len[fmt]
        if fmt in ('bl', 'str'):
            return 1 + payload[0][0] if payload else None
        if fmt is not None:
            return 2
        return None

    def value_text(self, addr, code, fmt, unit, vals):
        n = len(vals)
        word = vals[0] | (vals[1] << 8) if n >= 2 else None
        if fmt in ('bl', 'str'):
            if n == 0:
                return None
            cnt, blk = vals[0], vals[1:]
            if fmt == 'str':
                t = '"%s"' % ''.join(chr(c) if 32 <= c < 127 else '.' for c in blk)
            else:
                t = ' '.join('%02X' % c for c in blk)
            if cnt != len(blk):
                t += ' (count %d, got %d)' % (cnt, len(blk))
            return t
        if fmt in ('b', 'vm', 'sb') and n >= 1:
            v = vals[0]
            if fmt == 'vm':
                mode = v >> 5
                if mode == 0:
                    self.vout_exp[(addr, self.page.get(addr, 0))] = signed(v & 0x1F, 5)
                    return 'Linear, exponent %d' % signed(v & 0x1F, 5)
                self.vout_exp.pop((addr, self.page.get(addr, 0)), None)
                return ('VID', 'Direct', 'IEEE half')[mode - 1] if mode <= 3 else '0x%02X' % v
            if fmt == 'sb':
                return '0x%02X %s' % (v, ' '.join(bitnames(v, status_byte_bits)))
            if code == 0x00 and self.cmds is pmbus_cmds:
                self.page[addr] = v
            return '0x%02X' % v
        if word is None:
            return None
        if fmt == 'l11':
            return fmtnum(signed(word & 0x7FF, 11) * 2.0 ** signed(word >> 11, 5), unit)
        if fmt == 'l16':
            exp = self.vout_exp.get((addr, self.page.get(addr, 0)))
            if exp is None:
                return '0x%04X (VOUT_MODE unknown)' % word
            return fmtnum(word * 2.0 ** exp, unit)
        if fmt == 'sw':
            names = bitnames(word & 0xFF, status_byte_bits) + \
                    bitnames(word >> 8, status_word_bits)
            return '0x%04X %s' % (word, ' '.join(names))
        if fmt == 'u16':
            return fmtnum(word, unit)
        if fmt == 'i16':
            return fmtnum(signed(word, 16), unit)
        if fmt == 'dk':
            return '%.1f degC' % (word / 10.0 - 273.15)
        if fmt == 'bst':
            names = [nm for bit, nm in sorted(battery_status_bits.items(), reverse=True)
                     if word & (1 << bit)]
            return '0x%04X %s' % (word, ' '.join(names))
        if fmt == 'date':
            return '%04d-%02d-%02d' % (1980 + (word >> 9), (word >> 5) & 0x0F, word & 0x1F)
        return '0x%04X' % word

    def put_fields(self, cmd_byte, payload, fmt, pec_byte, pec_ok):
        if cmd_byte is not None:
            code, ss, es = cmd_byte
            name = self.cmds.get(code, (None,))[0]
            if name:
                self.putg(ss, es, 0, ['Command: %s (0x%02X)' % (name, code), name,
                                      '%02X' % code])
            else:
                self.putg(ss, es, 0, ['Command: 0x%02X' % code, '%02X' % code])
        for i, (v, ss, es) in enumerate(payload):
            if i == 0 and fmt in ('bl', 'str'):
                self.putg(ss, es, 1, ['Count: %d' % v, 'N=%d' % v, '%d' % v])
            else:
                self.putg(ss, es, 2, ['Data: 0x%02X' % v, '%02X' % v])
        if pec_byte is not None:
            v, ss, es = pec_byte
            if pec_ok:
                self.putg(ss, es, 3, ['PEC OK: 0x%02X' % v, 'PEC', 'P'])
            else:
                self.putg(ss, es, 3, ['PEC error: 0x%02X' % v, 'PEC!', 'E'])
                self.putg(ss, es, 5, ['PEC mismatch', 'PEC error'])

    def transaction(self, ss, es):
        segs = self.segs
        s0 = segs[0]
        addr = s0['addr']
        a = '0x%02X' % addr
        if s0['nack']:
            self.putg(ss, es, 4, ['%s: address NACK' % a, 'NACK'])
            return
        d0 = s0['data']

        # Special SMBus addresses.
        if len(segs) == 1 and addr == ADDR_ALERT_RESPONSE and s0['rw'] and d0:
            self.putg(ss, es, 4, ['Alert response: device 0x%02X' % (d0[0][0] >> 1),
                                  'ALERT 0x%02X' % (d0[0][0] >> 1)])
            return
        if len(segs) == 1 and addr == ADDR_HOST and not s0['rw'] and len(d0) >= 3:
            st = d0[1][0] | (d0[2][0] << 8)
            self.putg(ss, es, 4, ['Host notify from 0x%02X: 0x%04X' % (d0[0][0] >> 1, st),
                                  'Notify 0x%02X' % (d0[0][0] >> 1)])
            return

        if len(segs) == 1 and not d0:
            self.putg(ss, es, 4, ['%s: Quick command (%s)' % (a, 'R' if s0['rw'] else 'W'),
                                  'Quick'])
            return

        if len(segs) == 1 and s0['rw']:
            payload, pec_ok = self.split_pec(d0, 1)
            pec_b = d0[-1] if pec_ok is not None else None
            self.put_fields(None, payload, None, pec_b, pec_ok)
            val = ' '.join('0x%02X' % b[0] for b in payload)
            self.putg(ss, es, 4, ['%s: Receive byte: %s' % (a, val), 'Recv %s' % val])
            return

        code = d0[0][0]
        name, fmt, unit = self.cmds.get(code, ('0x%02X' % code, None, ''))
        label = name if name.startswith('0x') else '%s (0x%02X)' % (name, code)

        if len(segs) == 1:
            payload = d0[1:]
            payload, pec_ok = self.split_pec(payload, self.expected_len(fmt, payload))
            kind = self.kind('Write', fmt, payload)
            rd = []
        elif len(segs) == 2 and not s0['rw'] and segs[1]['rw'] and not segs[1]['nack']:
            wr = d0[1:]
            rd = segs[1]['data']
            rd, pec_ok = self.split_pec(rd, None if wr else self.expected_len(fmt, rd))
            payload = rd
            if wr:
                kind = 'Block process call' if fmt in ('bl', 'str') else 'Process call'
                self.put_fields(None, wr, fmt, None, None)
            else:
                kind = self.kind('Read', fmt, rd)
        else:
            parts = []
            for s in segs:
                parts.append('%s 0x%02X: %s' % ('R' if s['rw'] else 'W', s['addr'],
                             ' '.join('%02X' % b[0] for b in s['data'])))
            self.putg(ss, es, 4, ['I²C: ' + ' | '.join(parts), 'I²C'])
            return

        pec_src = segs[-1]['data']
        pec_b = pec_src[-1] if pec_ok is not None else None
        self.put_fields(d0[0], payload, fmt, pec_b, pec_ok)

        vals = [b[0] for b in payload]
        if fmt is not None:
            exp = self.expected_len(fmt, payload)
            if exp is not None and len(payload) != exp:
                self.putg(ss, es, 5, ['%s: expected %d data bytes, got %d'
                                      % (name, exp, len(payload)), 'Length'])
            v = self.value_text(addr, code, fmt, unit, vals)
        else:
            v = ' '.join('%02X' % x for x in vals) if vals else None
        t = '%s: %s %s' % (a, kind, label)
        if v:
            t += ': ' + v
        if pec_ok is not None:
            t += ' [PEC %s]' % ('OK' if pec_ok else 'error')
        self.putg(ss, es, 4, [t, '%s %s%s' % (name, v or '', '' if pec_ok is not False
                                               else ' PEC!'), name])

    def kind(self, rw, fmt, payload):
        if fmt == 's' or (rw == 'Write' and not payload):
            return 'Send byte'
        if fmt in ('bl', 'str'):
            return 'Block %s' % rw.lower()
        n = len(payload)
        if n == 1:
            return '%s byte' % rw
        if n == 2:
            return '%s word' % rw
        return '%s %d bytes' % (rw, n)
