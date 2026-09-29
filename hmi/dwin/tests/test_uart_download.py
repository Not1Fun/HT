import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / "tools/uart_download.py"
SPEC = importlib.util.spec_from_file_location("uart_download", SOURCE)
uart = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(uart)
TEST_TEMP = uart.ROOT / ".tools"
TEST_TEMP.mkdir(exist_ok=True)


class Port:
    def __init__(self, crc="none", ack=False):
        self.crc = crc
        self.ack = ack
        self.rx = bytearray()
        self.memory = bytearray(131072)
        self.sent = []
        self.flash = {}
        self.bad_read = None
        self.corrupt_reply = False
        self.drop_read = False
        self.drop_commit = False
        self.busy_forever = False
        self.wrong_commit = False
        self.next_status = None
        self.wrong_address = False
        self.memory[0x0F * 2:0x10 * 2] = bytes.fromhex("6322")
        self.memory[0x7A * 2:0x7C * 2] = struct.pack(">HH", 800, 480)
        self.memory[0x80 * 2:0x82 * 2] = bytes.fromhex("00141038")

    def write(self, packet):
        self.sent.append(bytes(packet))
        bodies = uart.FrameReader(self.crc).feed(packet)
        if not bodies:
            return len(packet)
        body = bodies[0]
        vp = int.from_bytes(body[1:3], "big")
        if body[0] == 0x82:
            data = body[3:]
            if vp == 0xAA and data[:2] == b"\x5a\x02":
                if self.drop_commit:
                    return len(packet)
                block = int.from_bytes(data[2:4], "big")
                self.flash[block] = bytes(self.memory[0x10000:0x18000])
                self.memory[vp * 2:vp * 2 + len(data)] = data
                if not self.busy_forever:
                    self.next_status = b"\x00" + data[1:]
                    if self.wrong_commit:
                        self.next_status = bytes(12)
            else:
                self.memory[vp * 2:vp * 2 + len(data)] = data
            if vp == 0x84:
                self.memory[0x14 * 2:0x15 * 2] = data[-2:]
            if self.ack:
                self.rx.extend(uart.frame(b"\x82OK", self.crc))
        elif body[0] == 0x83 and not self.drop_read:
            words = body[3]
            data = bytes(self.memory[vp * 2:vp * 2 + words * 2])
            if self.next_status is not None and vp == 0xAA:
                self.memory[0xAA * 2:0xAA * 2 + 12] = self.next_status
                self.next_status = None
            if self.bad_read == vp:
                data = bytes([data[0] ^ 1]) + data[1:]
            reply = body + data
            if self.wrong_address:
                reply = reply[:2] + bytes([reply[2] ^ 1]) + reply[3:]
            response = uart.frame(reply, self.crc)
            if self.corrupt_reply:
                response = response[:-1] + bytes([response[-1] ^ 1])
            self.rx.extend(response)
        return len(packet)

    def read(self, count):
        data = bytes(self.rx[:count])
        del self.rx[:count]
        return data

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.rx.clear()


def fixtures(directory):
    for name, _, _ in uart.RESOURCES:
        if name.endswith(".icl"):
            data = b"DGUS_3" + bytes(100)
        elif name == "14ShowFile.bin":
            data = b"\x14DGUS_2" + bytes(100)
        elif name == "22_Config.bin":
            data = bytearray(0x20004)
            data[0x2000:0x2002] = b"\x00\x04"
            data[0x2200:0x2202] = b"--"
        else:
            data = b"\xff\xff"
        (directory / name).write_bytes(data)


class DownloadTests(unittest.TestCase):
    def test_crc_and_request_frames(self):
        self.assertEqual(uart.crc16(b"123456789"), 0x4B37)
        self.assertEqual(uart.frame(bytes.fromhex("83000f01"), "none").hex(), "5aa50483000f01")
        self.assertEqual(uart.frame(bytes.fromhex("83000f01"), "modbus").hex(), "5aa50683000f01ed90")
        with self.assertRaises(ValueError):
            uart.frame(bytes(256), "none")

    def test_parser_fragments_noise_and_bad_crc(self):
        reader = uart.FrameReader("modbus")
        packet = uart.frame(bytes.fromhex("83000f016322"), "modbus")
        broken = bytearray(packet)
        broken[-1] ^= 1
        self.assertEqual(reader.feed(b"noise" + broken + packet[:4]), [])
        self.assertEqual(reader.feed(packet[4:]), [bytes.fromhex("83000f016322")])
        self.assertGreater(reader.bad_frames, 0)
        reader = uart.FrameReader("none")
        self.assertEqual(reader.feed(b"noise\x5a"), [])
        self.assertEqual(reader.feed(bytes.fromhex("a50683000f016322")), [bytes.fromhex("83000f016322")])

    def test_probe_both_modes_ack_optional_and_read_only(self):
        for crc in ("none", "modbus"):
            for ack in (False, True):
                port = Port(crc, ack)
                result = uart.probe(port, timeout=0.003)
                self.assertEqual(result["crc"], crc)
                self.assertEqual(result["gui_version_hex"], "63")
                self.assertEqual(result["resolution"], [800, 480])
                self.assertTrue(all(packet[3] == 0x83 for packet in port.sent))

    def test_read_rejects_timeout_bad_crc_and_wrong_shape(self):
        port = Port("modbus")
        port.corrupt_reply = True
        with self.assertRaises(uart.DownloadError):
            uart.Client(port, "modbus", 0.003).read(0xF, 1)
        port = Port()
        port.drop_read = True
        with self.assertRaises(uart.DownloadError):
            uart.Client(port, "none", 0.003).read(0xF, 1)
        port = Port()
        port.wrong_address = True
        with self.assertRaises(uart.DownloadError):
            uart.Client(port, "none", 0.003).read(0xF, 1)
        for vp, words in [(0, 0), (0, 121), (0xFFFF, 2), (-1, 1)]:
            with self.assertRaises(ValueError):
                uart.Client(port, "none").read(vp, words)
        port = Port()
        port.memory[0x7A * 2:0x7C * 2] = bytes(4)
        with self.assertRaises(uart.DownloadError):
            uart.identify(uart.Client(port, "none"))

    def test_native_portrait_panel_with_landscape_rotation(self):
        for width, height in ((480, 800), (800, 480)):
            for rotation in range(4):
                with self.subTest(width=width, height=height, rotation=rotation):
                    port = Port()
                    port.memory[0x7A * 2:0x7C * 2] = struct.pack(">HH", width, height)
                    port.memory[0x80 * 2:0x82 * 2] = bytes([0, 0x14, 0xF0, 0x38 | rotation])
                    client = uart.Client(port, "none")
                    if (width == 480) == bool(rotation % 2):
                        result = uart.identify(client)
                        self.assertEqual(result["resolution"], [800, 480])
                        self.assertEqual(result["native_resolution"], [width, height])
                        self.assertEqual(result["rotation_degrees"], rotation * 90)
                    else:
                        with self.assertRaises(uart.DownloadError):
                            uart.identify(client)
                    self.assertTrue(all(packet[3] == 0x83 for packet in port.sent))

    def test_plan_allowlist_boundaries_and_22_no_prefix_removal(self):
        with tempfile.TemporaryDirectory(dir=TEST_TEMP) as temp:
            directory = Path(temp)
            fixtures(directory)
            (directory / "T5LCFG.CFG").write_bytes(b"do not install")
            (directory / "T5L_OS.bin").write_bytes(b"do not install")
            plan = uart.make_plan(directory, keep_font=True)
            self.assertEqual(len(plan["files"]), 6)
            self.assertEqual([f["id"] for f in plan["files"]], [32, 42, 0, 22, 13, 14])
            self.assertFalse(plan["files"][2]["install"])
            file22 = plan["files"][3]
            self.assertEqual(file22["block_start"], 0xB0)
            self.assertEqual(file22["block_count"], 5)
            self.assertEqual(file22["size"], 131076)
            chunk22 = next(uart.blocks((directory / "22_Config.bin").read_bytes()))
            self.assertEqual(chunk22[0x2000:0x2002], b"\x00\x04")
            self.assertEqual(chunk22[0x2200:0x2202], b"--")
            (directory / "13TouchFile.bin").write_bytes(bytes(uart.SLOT_SIZE + 1))
            with self.assertRaises(uart.DownloadError):
                uart.make_plan(directory)

    def test_padding_whole_block_including_last_byte(self):
        self.assertEqual(list(uart.blocks(b"")), [])
        for length, count in [(2, 1), (32768, 1), (32769, 2), (65536, 2)]:
            data = bytes((i % 251 for i in range(length)))
            chunks = list(uart.blocks(data))
            self.assertEqual(len(chunks), count)
            joined = b"".join(chunks)
            self.assertEqual(joined[:length], data)
            self.assertEqual(joined[length:], b"\xff" * (count * uart.BLOCK_SIZE - length))

    def test_ram_roundtrip_all_bytes_and_crc_modes(self):
        data = bytes(i % 251 for i in range(uart.BLOCK_SIZE))
        for crc in ("none", "modbus"):
            port = Port(crc, ack=True)
            uart.stage_ram(uart.Client(port, crc), data)
            self.assertEqual(port.memory[0x10000:0x18000], data)
            self.assertFalse(port.flash)
            writes = [p for p in port.sent if p[3] == 0x82]
            reads = [p for p in port.sent if p[3] == 0x83]
            self.assertEqual(len(writes), 137)
            self.assertEqual(len(reads), 137)
            self.assertEqual(int.from_bytes(reads[-1][4:6], "big"), 0xBFC0)
            self.assertEqual(reads[-1][6], 64)

    def test_ram_mismatch_never_reaches_flash(self):
        port = Port()
        port.bad_read = 0x8000 + 120
        with self.assertRaisesRegex(uart.DownloadError, "未提交Flash"):
            uart.stage_ram(uart.Client(port, "none"), bytes(uart.BLOCK_SIZE))
        self.assertFalse(port.flash)
        self.assertFalse(any(p[3:6] == bytes.fromhex("8200aa") for p in port.sent))

    def test_commit_exact_descriptor_and_busy_complete(self):
        port = Port()
        client = uart.Client(port, "none")
        uart.prepare_commit(client)
        with patch.object(uart.time, "sleep"):
            uart.commit_block(client, 0x100)
        self.assertIn(bytes.fromhex("5aa50f8200aa5a0201008000001400000000"), port.sent)
        self.assertIn(0x100, port.flash)
        with self.assertRaises(ValueError):
            uart.commit_block(client, 512)

    def test_existing_busy_stale_completion_drop_and_wrong_descriptor(self):
        port = Port()
        port.memory[0xAA * 2] = 0x5A
        with self.assertRaisesRegex(uart.DownloadError, "已有Flash操作"):
            uart.prepare_commit(uart.Client(port, "none"))
        self.assertTrue(all(p[3] == 0x83 for p in port.sent))
        for failure in ("drop_commit", "wrong_commit", "busy_forever"):
            port = Port()
            port.memory[0xAA * 2:0xAA * 2 + 12] = struct.pack(">BBHHHI", 0, 2, 0x100, 0x8000, 20, 0)
            client = uart.Client(port, "none")
            uart.prepare_commit(client)
            setattr(port, failure, True)
            with self.assertRaises(uart.DownloadError), patch.object(uart.time, "sleep"):
                uart.commit_block(client, 0x100, timeout=0.002)

    def test_install_failure_journal_no_reset_and_success_proof(self):
        with tempfile.TemporaryDirectory(dir=TEST_TEMP) as temp:
            base = Path(temp)
            directory = base / "input"
            directory.mkdir()
            fixtures(directory)
            plan = uart.make_plan(directory, keep_font=True)
            with patch.object(uart, "ROOT", base), patch.object(uart.time, "sleep"):
                port = Port()
                port.bad_read = 0x8000
                journal = uart.Journal(base / ".tools/fail.json", plan, "fake", "none")
                with self.assertRaises(uart.DownloadError):
                    uart.install(uart.Client(port, "none"), plan, journal, announce=lambda _: None)
                saved = json.loads(journal.path.read_text(encoding="utf-8"))
                self.assertEqual(saved["stage"], "failed")
                self.assertFalse(port.flash)
                self.assertFalse(any(p[3:6] == bytes.fromhex("820004") for p in port.sent))
                port = Port()
                journal = uart.Journal(base / ".tools/ok.json", plan, "fake", "none")
                result = uart.install(uart.Client(port, "none"), plan, journal, announce=lambda _: None)
                self.assertEqual(result["stage"], "complete")
                self.assertFalse(result["flash_content_readback"])
                self.assertEqual(result["displayed_page"], 0)
                self.assertEqual(list(port.flash), [0x100, 0x150, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0x68, 0x70])
                self.assertEqual(port.memory[0xFC * 2:0xFC * 2 + 4], bytes.fromhex("55aa5aa5"))
                self.assertEqual(port.memory[0xDE * 2:0xDE * 2 + 4], bytes.fromhex("5a000020"))
                self.assertFalse(any(p[3:6] in [bytes.fromhex("820080"), bytes.fromhex("82000c"), bytes.fromhex("820006")] for p in port.sent))

    def test_install_rejects_existing_busy_before_ram_or_flash_write(self):
        with tempfile.TemporaryDirectory(dir=TEST_TEMP) as temp:
            base = Path(temp)
            fixtures(base)
            plan = uart.make_plan(base, keep_font=True)
            with patch.object(uart, "ROOT", base):
                port = Port()
                port.memory[0xAA * 2] = 0x5A
                journal = uart.Journal(base / ".tools/busy.json", plan, "fake", "none")
                with self.assertRaisesRegex(uart.DownloadError, "已有Flash操作"):
                    uart.install(uart.Client(port, "none"), plan, journal, announce=lambda _: None)
                self.assertTrue(all(p[3] == 0x83 for p in port.sent))

    def test_journal_confinement_and_refuse_overwrite(self):
        with tempfile.TemporaryDirectory(dir=TEST_TEMP) as temp, patch.object(uart, "ROOT", Path(temp)):
            with self.assertRaises(uart.DownloadError):
                uart.Journal(Path(temp) / "outside.json", {}, "fake", "none")
            path = Path(temp) / ".tools/progress.json"
            uart.Journal(path, {}, "fake", "none")
            with self.assertRaises(uart.DownloadError):
                uart.Journal(path, {}, "fake", "none")


if __name__ == "__main__":
    unittest.main()
