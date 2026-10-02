"""HT 单槽 MCUboot 串口恢复：应答及 SHA256 验证通过后才复位。

协议依据固定 MCUboot v2.4.0 boot/boot_serial/src/boot_serial.c：
https://github.com/zephyrproject-rtos/mcuboot/tree/6d3b3d2c38ab20c242e5b9abb04d050086383eb2
SHA256 检查完整性，不提供签名认证；单槽更新没有旧应用回滚副本。
"""

import argparse
import base64
import binascii
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path
import secrets
import struct
import sys
import time

import cbor2
import serial


SLOT_ADDRESS = 0x08010000
SLOT_SIZE = 0x70000
HEADER_SIZE = 0x200
MAX_PACKET = 1024
LINE_DATA = 124  # 2-byte NLIP prefix + base64 + LF <= 127 bytes.
START = b"\x06\x09"
CONTINUE = b"\x04\x14"


class UpdateError(Exception):
    """协议、镜像或设备校验失败；不得继续写入。"""


class ReplyTimeout(UpdateError):
    """未收到完整应答。"""


@dataclass(frozen=True)
class Image:
    data: bytes
    digest: bytes


def inspect_image(data):
    """只接受本项目固定分区、0x200 头、无签名 SHA256 镜像。"""
    if not 32 <= len(data) <= SLOT_SIZE:
        raise UpdateError("镜像大小不在应用槽范围内")
    header = struct.unpack_from("<IIHHIIBBHII", data)
    magic, load, head, protected, size, flags = header[:6]
    if magic != 0x96F3B83D or load or head != HEADER_SIZE or protected or flags:
        raise UpdateError("不是 HT 固定槽的无签名 MCUboot 镜像头")
    end = head + size
    if size < 8 or end + 40 != len(data):
        raise UpdateError("镜像正文或 SHA256 TLV 长度不符")
    if struct.unpack_from("<HHBBH", data, end) != (0x6907, 40, 0x10, 0, 32):
        raise UpdateError("镜像必须恰含一个 SHA256 TLV")
    digest = hashlib.sha256(data[:end]).digest()
    if digest != data[end + 8:]:
        raise UpdateError("本地镜像 SHA256 校验失败")
    stack, reset = struct.unpack_from("<II", data, head)
    if not 0x20000000 < stack <= 0x20020000 or stack % 8:
        raise UpdateError("初始栈不在 G474 SRAM 中或未对齐")
    if not reset & 1 or not SLOT_ADDRESS + head <= (reset & ~1) < SLOT_ADDRESS + end:
        raise UpdateError("应用入口未链接到 0x08010200 应用槽")
    return Image(bytes(data), digest)


def encode_packet(packet):
    if not 8 <= len(packet) <= MAX_PACKET:
        raise UpdateError("SMP 数据包长度越界")
    crc = binascii.crc_hqx(packet, 0)
    encoded = base64.b64encode(struct.pack(">H", len(packet) + 2) + packet +
                               struct.pack(">H", crc))
    return b"".join((START if offset == 0 else CONTINUE) +
                    encoded[offset:offset + LINE_DATA] + b"\n"
                    for offset in range(0, len(encoded), LINE_DATA))


class Decoder:
    """有界 NLIP 流解码；普通日志丢弃，协议损坏立即报错。"""

    def __init__(self):
        self.line = bytearray()
        self.packet = bytearray()
        self.expected = None
        self.discard = False

    def feed(self, data):
        packets = []
        for byte in data:
            if byte != 10:
                if not self.discard:
                    self.line.append(byte)
                    if len(self.line) > LINE_DATA + 2:
                        if self.line[:2] in (START, CONTINUE):
                            raise UpdateError("NLIP 行超过 127 字节")
                        self.line.clear()
                        self.discard = True
                continue
            if not self.discard:
                result = self._line(bytes(self.line))
                if result is not None:
                    packets.append(result)
            self.line.clear()
            self.discard = False
        return packets

    def _line(self, line):
        prefix = line[:2]
        if prefix not in (START, CONTINUE):
            return None
        if prefix == START:
            if self.packet:
                raise UpdateError("尚未完成的 NLIP 包被新包覆盖")
            self.expected = None
        elif self.expected is None:
            raise UpdateError("NLIP 续帧缺少首帧")
        try:
            decoded = base64.b64decode(line[2:], validate=True)
        except (ValueError, binascii.Error) as error:
            raise UpdateError("NLIP base64 损坏") from error
        if not decoded:
            raise UpdateError("NLIP 空帧")
        self.packet.extend(decoded)
        if self.expected is None:
            if len(self.packet) < 2:
                raise UpdateError("NLIP 首帧缺少长度")
            self.expected = struct.unpack_from(">H", self.packet)[0] + 2
            if not 12 <= self.expected <= MAX_PACKET + 4:
                raise UpdateError("NLIP 包长度越界")
        if len(self.packet) > self.expected:
            raise UpdateError("NLIP 数据超出声明长度")
        if len(self.packet) < self.expected:
            return None
        if binascii.crc_hqx(self.packet[2:], 0) != 0:
            raise UpdateError("NLIP CRC16 校验失败")
        result = bytes(self.packet[2:-2])
        self.packet.clear()
        self.expected = None
        return result


def unpack_reply(packet, identity):
    if len(packet) < 8:
        raise UpdateError("SMP 应答缺少头")
    op, flags, size, group, sequence, command = struct.unpack_from(">BBHHBB", packet)
    if (op, group, sequence, command) != identity or flags or size != len(packet) - 8:
        raise UpdateError("SMP 应答标识或长度不符")
    stream = io.BytesIO(packet[8:])
    try:
        payload = cbor2.load(stream)
    except (ValueError, cbor2.CBORDecodeError) as error:
        raise UpdateError("SMP CBOR 应答损坏") from error
    if stream.read(1) or not isinstance(payload, dict):
        raise UpdateError("SMP 应答不是单一 CBOR 对象")
    if "err" in payload or ("rc" in payload and
                            (type(payload["rc"]) is not int or payload["rc"] != 0)):
        raise UpdateError(f"设备拒绝命令: {payload}")
    return payload


class Client:
    def __init__(self, port, timeout=3.0, clock=time.monotonic):
        self.port = port
        self.timeout = timeout
        self.clock = clock
        self.sequence = 0
        self.decoder = Decoder()

    def request(self, op, group, command, payload):
        body = cbor2.dumps(payload)
        sequence = self.sequence
        self.sequence = (sequence + 1) & 255
        packet = struct.pack(">BBHHBB", op, 0, len(body), group, sequence, command) + body
        frame = encode_packet(packet)
        if self.port.write(frame) != len(frame):
            raise UpdateError("串口未完整发送，停止升级")
        deadline = self.clock() + self.timeout
        while self.clock() < deadline:
            data = self.port.read(256)
            replies = self.decoder.feed(data)
            if replies:
                if len(replies) != 1 or self.decoder.packet or self.decoder.line:
                    raise UpdateError("收到重复应答或额外不完整数据")
                return unpack_reply(replies[0], (op + 1, group, sequence, command))
        raise ReplyTimeout("等待串口应答超时；未确认写入，停止升级")

    def connect(self, wait=15.0):
        """只重试无副作用 echo；Flash 写入从不盲重试。"""
        deadline = self.clock() + wait
        while True:
            nonce = secrets.token_hex(8)
            try:
                response = self.request(2, 0, 0, {"d": nonce})
                if response.get("r") != nonce:
                    raise UpdateError("echo 内容不符")
                break
            except ReplyTimeout:
                if self.clock() >= deadline:
                    raise UpdateError("未建立双向串口连接；没有发送任何 Flash 上传命令")
                self.decoder = Decoder()
        return self.images()

    def images(self):
        response = self.request(0, 1, 0, {})
        images = response.get("images")
        if not isinstance(images, list) or len(images) > 1:
            raise UpdateError("设备不是预期单槽 MCUboot")
        for image in images:
            if (not isinstance(image, dict) or type(image.get("slot")) is not int or
                    image["slot"] != 0 or not isinstance(image.get("hash"), bytes) or
                    len(image["hash"]) != 32):
                raise UpdateError("设备镜像列表/散列格式不符")
        return images

    def upload(self, image, wait=15.0, progress=None):
        # 对传入对象也再核对，避免库调用绕开本地镜像检查。
        image = inspect_image(image.data)
        self.connect(wait)
        offset = 0
        while offset < len(image.data):
            chunk = image.data[offset:offset + 256]
            payload = {"off": offset, "data": chunk}
            if offset == 0:
                payload["len"] = len(image.data)
            response = self.request(2, 1, 1, payload)
            expected = offset + len(chunk)
            if (response.get("rc") != 0 or type(response.get("off")) is not int or
                    response["off"] != expected):
                raise UpdateError(f"上传偏移未精确确认: 期望 {expected}，应答 {response}")
            offset = expected
            if progress:
                progress(offset, len(image.data))
        images = self.images()
        if len(images) != 1 or images[0]["hash"] != image.digest:
            raise UpdateError("设备完整镜像校验失败或 SHA256 不符；不发送复位")
        response = self.request(2, 0, 5, {})
        if response.get("rc") != 0:
            raise UpdateError("设备未确认复位")
        return image.digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="例如 COM16")
    parser.add_argument("--wait", type=float, default=15, help="等待人工复位进入恢复的秒数")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("probe", help="只读握手和镜像校验列表，不写 Flash")
    upload = commands.add_parser("upload", help="上传 app zephyr.signed.bin，校验后复位")
    upload.add_argument("image", type=Path)
    args = parser.parse_args(argv)
    if not 0 < args.wait <= 120:
        parser.error("--wait 必须在 0 到 120 秒之间")
    try:
        image = inspect_image(args.image.read_bytes()) if args.command == "upload" else None
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = args.port
        with port:
            port.reset_input_buffer()
            client = Client(port)
            print(f"等待 MCUboot 双向应答，请在 {args.wait:g} 秒内按主板复位。", flush=True)
            if image:
                def progress(done, total):
                    print(f"\r已确认 {done}/{total} 字节", end="", flush=True)
                digest = client.upload(image, args.wait, progress)
                print(f"\n设备 SHA256 校验和复位应答通过: {digest.hex()}")
                print("应用是否已正常运行仍需另行确认。")
            else:
                images = client.connect(args.wait)
                for item in images:
                    print(f"slot0 SHA256: {item['hash'].hex()}")
                if not images:
                    print("串口恢复连接正常；没有通过校验的应用镜像。")
                print("probe 已使引导保持恢复模式；手动复位可退出。")
        return 0
    except (UpdateError, OSError, serial.SerialException) as error:
        print(f"\n失败: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
