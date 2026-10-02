"""MCUboot 串口升级工具的镜像、线协议和故障停止测试；不打开硬件。"""
import base64
import binascii
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

import cbor2


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "uart_update.py"
SPEC = importlib.util.spec_from_file_location("uart_update", SCRIPT)
update = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = update
SPEC.loader.exec_module(update)


def make_image(**changes):
    values = {"magic": 0x96F3B83D, "load": 0, "head": 0x200, "protected": 0,
              "size": 600, "flags": 0, "stack": 0x20020000, "reset": 0x08010209}
    values.update(changes)
    header = struct.pack("<IIHHIIBBHII", values["magic"], values["load"],
                         values["head"], values["protected"], values["size"],
                         values["flags"], 1, 0, 0, 1, 0)
    body = header + bytes(0x200 - len(header))
    body += struct.pack("<II", values["stack"], values["reset"]) + bytes(592)
    return body + struct.pack("<HHBBH", 0x6907, 40, 0x10, 0, 32) + hashlib.sha256(body).digest()


class Clock:
    now = 0.0

    def __call__(self):
        return self.now


class Device:
    """真实 NLIP/CBOR 线格式设备替身，仅模拟 Flash，不调用串口。"""
    def __init__(self, mode=None):
        self.mode = mode
        self.clock = Clock()
        self.requests = []
        self.pending = bytearray()
        self.flash = bytearray()
        self.reset = False
        self.fragment = 7

    def write(self, frame):
        self.assert_lines(frame)
        packet, = update.Decoder().feed(frame)
        op, _, size, group, sequence, command = struct.unpack_from(">BBHHBB", packet)
        assert len(packet) == size + 8
        body = cbor2.loads(packet[8:])
        self.requests.append((group, command, body))
        if self.mode == "short_write":
            return len(frame) - 1
        if self.mode == "no_echo" and (group, command) == (0, 0):
            return len(frame)
        if (group, command) == (0, 0):
            response = {"r": body["d"] if self.mode != "bad_echo" else "wrong"}
        elif (group, command) == (1, 0):
            images = []
            if self.flash:
                digest = update.inspect_image(bytes(self.flash)).digest
                if self.mode == "wrong_hash":
                    digest = bytes(32)
                images = [{"slot": 0, "hash": digest}]
            response = {"images": images}
            if self.mode == "wrong_slot":
                response = {"images": [{"slot": 1, "hash": bytes(32)}]}
            if self.mode == "bad_images":
                response = {"images": {}}
        elif (group, command) == (1, 1):
            assert body["off"] == len(self.flash)
            if not self.flash:
                self.total = body["len"]
            self.flash.extend(body["data"])
            response = {"rc": 0, "off": len(self.flash)}
            if self.mode == "no_upload_ack":
                return len(frame)
            if self.mode in ("offset_back", "offset_jump", "offset_same"):
                response["off"] = {"offset_back": -1, "offset_jump": len(self.flash) + 1,
                                   "offset_same": body["off"]}[self.mode]
            if self.mode == "offset_bool":
                response["off"] = True
            if self.mode == "device_error":
                response["rc"] = 2
            if self.mode == "missing_rc":
                del response["rc"]
            if self.mode == "bool_rc":
                response["rc"] = False
        elif (group, command) == (0, 5):
            assert len(self.flash) == self.total
            self.reset = True
            response = {"rc": 0}
            if self.mode == "no_reset_ack":
                return len(frame)
        else:
            raise AssertionError("unexpected command")
        payload = cbor2.dumps(response)
        header = [op + 1, 0, len(payload), group, sequence, command]
        if self.mode == "wrong_sequence":
            header[4] ^= 1
        if self.mode == "wrong_op":
            header[0] ^= 1
        if self.mode == "wrong_command":
            header[5] ^= 1
        if self.mode == "wrong_group":
            header[3] ^= 1
        self.pending.extend(update.encode_packet(struct.pack(">BBHHBB", *header) + payload))
        return len(frame)

    @staticmethod
    def assert_lines(frame):
        assert all(len(line) <= 127 for line in frame.splitlines(keepends=True))

    def read(self, count):
        self.clock.now += 0.01
        count = min(count, self.fragment)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    @property
    def uploads(self):
        return [body for group, command, body in self.requests if (group, command) == (1, 1)]


class ImageTests(unittest.TestCase):
    def test_valid_image(self):
        raw = make_image()
        image = update.inspect_image(raw)
        self.assertEqual(image.data, raw)
        self.assertEqual(image.digest, hashlib.sha256(raw[:-40]).digest())

    def test_reject_header_and_vector_variants(self):
        for changes in ({"magic": 0}, {"load": 0x8000000}, {"head": 0x100},
                        {"protected": 32}, {"size": 1}, {"size": 599}, {"flags": 4},
                        {"stack": 0x20000000}, {"stack": 0x20020008},
                        {"stack": 0x20000101}, {"reset": 0x08000201},
                        {"reset": 0x08010208}, {"reset": 0x080fffff}):
            with self.subTest(changes=changes), self.assertRaises(update.UpdateError):
                update.inspect_image(make_image(**changes))

    def test_reject_truncated_extra_large_and_corrupt(self):
        raw = make_image()
        corrupt = bytearray(raw)
        corrupt[520] ^= 1
        bad_tlv = bytearray(raw)
        bad_tlv[-36] = 0x11
        for data in (b"", raw[:31], raw[:-1], raw + b"\0", bytes(0x70001), corrupt, bad_tlv):
            with self.subTest(length=len(data)), self.assertRaises(update.UpdateError):
                update.inspect_image(data)


class WireTests(unittest.TestCase):
    def test_crc_golden_and_fragmented_round_trip(self):
        self.assertEqual(binascii.crc_hqx(b"123456789", 0), 0x31C3)
        packet = bytes(range(256)) * 4
        wire = update.encode_packet(packet)
        Device.assert_lines(wire)
        for step in (1, 7, 93, 300, len(wire)):
            decoder = update.Decoder()
            received = []
            for offset in range(0, len(wire), step):
                received.extend(decoder.feed(wire[offset:offset + step]))
            self.assertEqual(received, [packet])

    def test_noise_is_bounded_and_packets_can_stick(self):
        decoder = update.Decoder()
        self.assertEqual(decoder.feed(b"x" * 20000), [])
        self.assertLessEqual(len(decoder.line), 126)
        packet = bytes(8)
        self.assertEqual(decoder.feed(b"\n" + update.encode_packet(packet) * 2), [packet] * 2)

    def test_bad_base64_crc_length_and_continuation(self):
        bad_crc = b"\x00\x0a" + bytes(8) + b"\x00\x01"
        for wire in (update.START + b"!!!\n", update.CONTINUE + b"AAAA\n",
                     update.START + base64.b64encode(bad_crc) + b"\n",
                     update.START + base64.b64encode(b"\xff\xff\0") + b"\n",
                     update.START + base64.b64encode(b"\0\x09" + bytes(10)) + b"\n",
                     update.START + b"A" * 125 + b"\n", update.START + b"\n"):
            with self.subTest(wire=wire), self.assertRaises(update.UpdateError):
                update.Decoder().feed(wire)

    def test_packet_replacement_and_incomplete_packet(self):
        first_line = update.encode_packet(bytes(200)).splitlines(keepends=True)[0]
        decoder = update.Decoder()
        self.assertEqual(decoder.feed(first_line), [])
        with self.assertRaises(update.UpdateError):
            decoder.feed(first_line)

    def test_reply_cbor_and_header(self):
        identity = (3, 0, 0, 0)
        for body in (b"\xff", cbor2.dumps([]), cbor2.dumps({}) + b"\0",
                     cbor2.dumps({"rc": False}), cbor2.dumps({"err": {"rc": 1}})):
            packet = struct.pack(">BBHHBB", 3, 0, len(body), 0, 0, 0) + body
            with self.subTest(body=body), self.assertRaises(update.UpdateError):
                update.unpack_reply(packet, identity)
        for packet in (b"", struct.pack(">BBHHBB", 3, 0, 2, 0, 0, 0) + b"\xa0"):
            with self.assertRaises(update.UpdateError):
                update.unpack_reply(packet, identity)


class UploadTests(unittest.TestCase):
    def run_upload(self, device):
        client = update.Client(device, timeout=0.5, clock=device.clock)
        return client.upload(update.inspect_image(make_image()), wait=1)

    def test_success_exact_bytes_and_reset_after_hash(self):
        device = Device()
        self.assertEqual(self.run_upload(device), update.inspect_image(make_image()).digest)
        self.assertEqual(bytes(device.flash), make_image())
        self.assertTrue(device.reset)
        self.assertEqual([(g, c) for g, c, _ in device.requests[-2:]], [(1, 0), (0, 5)])
        self.assertEqual([p["off"] for p in device.uploads], [0, 256, 512, 768, 1024])

    def test_no_echo_never_writes_flash(self):
        device = Device("no_echo")
        with self.assertRaisesRegex(update.UpdateError, "没有发送任何 Flash"):
            self.run_upload(device)
        self.assertGreater(len(device.requests), 1)
        self.assertEqual(device.uploads, [])

    def test_wrong_echo_protocol_or_list_never_writes_flash(self):
        for mode in ("bad_echo", "wrong_sequence", "wrong_op", "wrong_group",
                     "wrong_command", "short_write", "wrong_slot", "bad_images"):
            device = Device(mode)
            with self.subTest(mode=mode), self.assertRaises(update.UpdateError):
                self.run_upload(device)
            self.assertEqual(device.uploads, [])

    def test_upload_ack_failures_never_retry_or_reset(self):
        for mode in ("no_upload_ack", "offset_back", "offset_jump", "offset_same",
                     "offset_bool", "device_error", "missing_rc", "bool_rc"):
            device = Device(mode)
            with self.subTest(mode=mode), self.assertRaises(update.UpdateError):
                self.run_upload(device)
            self.assertEqual(len(device.uploads), 1)
            self.assertFalse(device.reset)

    def test_hash_mismatch_does_not_reset(self):
        device = Device("wrong_hash")
        with self.assertRaisesRegex(update.UpdateError, "SHA256"):
            self.run_upload(device)
        self.assertEqual(bytes(device.flash), make_image())
        self.assertFalse(device.reset)

    def test_no_reset_ack_is_not_success(self):
        with self.assertRaises(update.ReplyTimeout):
            self.run_upload(Device("no_reset_ack"))

    def test_invalid_image_does_not_even_handshake(self):
        device = Device()
        with self.assertRaises(update.UpdateError):
            update.Client(device).upload(update.Image(b"bad", bytes(32)))
        self.assertEqual(device.requests, [])


if __name__ == "__main__":
    unittest.main()
