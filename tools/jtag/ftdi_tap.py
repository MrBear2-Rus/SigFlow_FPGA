"""Bounded, LSB-first JTAG scans via FT2232H channel A (WinUSB/libftdi).

No hardware is opened at import. Only the documented read/user instructions
are allowed; configuration/erase instructions are deliberately excluded.
"""
import ctypes
import os
from pathlib import Path
import time

ALLOWED_IR = {0x11, 0x13, 0x41, 0x42, 0x43, 0xFF}


class FtdiTap:
    def __init__(self, dll_dir, hz=1000000):
        if not 458 <= hz <= 30000000:
            raise ValueError("TCK must be 458..30000000 Hz")
        self.dll_search = os.add_dll_directory(str(Path(dll_dir).resolve()))
        self.ft = ctypes.CDLL(str(Path(dll_dir) / "libftdi1.dll"))
        self.ft.ftdi_new.restype = ctypes.c_void_p
        specs = {
            "ftdi_set_interface": [ctypes.c_void_p, ctypes.c_int],
            "ftdi_usb_open": [ctypes.c_void_p, ctypes.c_int, ctypes.c_int],
            "ftdi_set_bitmode": [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_ubyte],
            "ftdi_set_latency_timer": [ctypes.c_void_p, ctypes.c_ubyte],
            "ftdi_usb_purge_buffers": [ctypes.c_void_p],
            "ftdi_write_data": [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int],
            "ftdi_read_data": [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int],
            "ftdi_usb_close": [ctypes.c_void_p],
            "ftdi_free": [ctypes.c_void_p],
            "ftdi_get_error_string": [ctypes.c_void_p],
        }
        for name, args in specs.items():
            getattr(self.ft, name).argtypes = args
            getattr(self.ft, name).restype = ctypes.c_int
        self.ft.ftdi_free.restype = None
        self.ft.ftdi_get_error_string.restype = ctypes.c_char_p
        self.dev = self.ft.ftdi_new()
        if not self.dev:
            raise RuntimeError("ftdi_new failed")
        try:
            self.check(self.ft.ftdi_set_interface(self.dev, 1))  # A BEFORE open
            self.check(self.ft.ftdi_usb_open(self.dev, 0x0403, 0x6010))
            self.check(self.ft.ftdi_set_latency_timer(self.dev, 1))
            self.check(self.ft.ftdi_set_bitmode(self.dev, 0, 0))
            self.check(self.ft.ftdi_set_bitmode(self.dev, 0xFB, 2))
            self.check(self.ft.ftdi_usb_purge_buffers(self.dev))
            self.write(bytes([0xAA, 0x87]))
            if self.read(2) != b"\xfa\xaa":
                raise RuntimeError("MPSSE sync failed")
            divisor = max(0, min(65535, (30000000 + hz - 1) // hz - 1))
            self.actual_hz = 30000000 // (divisor + 1)
            self.write(bytes([0x8A, 0x97, 0x8D, 0x85, 0x86,
                              divisor & 255, divisor >> 8,
                              0x80, 0x08, 0x0B]))
            self.reset()
        except Exception:
            self.close()
            raise

    def check(self, result):
        if result < 0:
            raise RuntimeError(self.ft.ftdi_get_error_string(self.dev).decode(errors="replace"))
        return result

    def write(self, data):
        buf = ctypes.create_string_buffer(data)
        n = self.check(self.ft.ftdi_write_data(self.dev, buf, len(data)))
        if n != len(data):
            raise IOError(f"short FTDI write: {n}/{len(data)}")

    def read(self, count):
        result = bytearray()
        deadline = time.monotonic() + 2
        while len(result) < count:
            buf = ctypes.create_string_buffer(count - len(result))
            n = self.check(self.ft.ftdi_read_data(self.dev, buf, len(buf)))
            result.extend(buf.raw[:n])
            if len(result) == count:
                return bytes(result)
            if time.monotonic() >= deadline:
                raise TimeoutError(f"FTDI read: {len(result)}/{count} bytes")
            time.sleep(0.001)

    def tms(self, bits, tdi=0):
        command = bytearray()
        for start in range(0, len(bits), 6):
            chunk = bits[start:start + 6]
            data = (tdi << 7) | (chunk[-1] << len(chunk))
            data |= sum(b << i for i, b in enumerate(chunk))
            command.extend([0x4B, len(chunk) - 1, data])
        self.write(bytes(command))

    def reset(self):
        self.tms([1] * 6 + [0, 0, 0, 0])

    def scan(self, nbits, value):
        """Start in Shift-IR/DR, exit on LAST bit, finish in Run-Test-Idle.

        MPSSE bit reads are MSB aligned; byte reads are naturally LSB-first.
        The final TMS/read command returns its single sample in bit 7.
        """
        if nbits < 1 or value < 0 or value >= (1 << nbits):
            raise ValueError("invalid scan length/value")
        full, rem = divmod(nbits - 1, 8)
        command = bytearray()
        if full:
            if full > 65536:
                raise ValueError("scan too long")
            command.extend([0x39, (full - 1) & 255, (full - 1) >> 8])
            command.extend((value & ((1 << (8 * full)) - 1)).to_bytes(full, "little"))
        if rem:
            command.extend([0x3B, rem - 1, (value >> (8 * full)) & 255])
        command.extend([0x6B, 0, 0x03 | (((value >> (nbits - 1)) & 1) << 7)])
        # Exit1 -> Update -> RTI, then 3 idle clocks. Send Immediate drains RX.
        command.extend([0x4B, 4, 0x01, 0x87])
        self.write(bytes(command))
        raw = self.read(full + bool(rem) + 1)
        result = int.from_bytes(raw[:full], "little")
        if rem:
            result |= (raw[full] >> (8 - rem)) << (full * 8)
        result |= (raw[-1] >> 7) << (nbits - 1)
        return result

    def ir(self, instruction):
        if instruction not in ALLOWED_IR:
            raise ValueError(f"IR 0x{instruction:02x} is outside read/user allowlist")
        self.tms([1, 1, 0, 0])
        return self.scan(8, instruction)

    def dr(self, nbits=32, value=0):
        self.tms([1, 0, 0])
        return self.scan(nbits, value)

    def close(self):
        if self.dev:
            self.ft.ftdi_usb_close(self.dev)
            self.ft.ftdi_free(self.dev)
            self.dev = None
        self.dll_search.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
