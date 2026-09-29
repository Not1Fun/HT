#!/usr/bin/env python3
"""HT DGUSII UART2 resource installer; never downloads CFG or kernel files."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SET = ROOT / "hmi/dwin/project/DWIN_SET"
BLOCK_SIZE = 32768
SLOT_SIZE = 262144
RAM_VP = 0x8000
CHUNK_SIZE = 240
RESOURCES = (
    ("32.icl", 32, 10),
    ("42.icl", 42, 22),
    ("0_DWIN_ASC.HZK", 0, 12),
    ("22_Config.bin", 22, 1),
    ("13TouchFile.bin", 13, 1),
    ("14ShowFile.bin", 14, 1),
)
FLASH_NOTICE = (
    "每个文件最后一个32KiB块的余部将写FF；不保留该块旧尾部。"
    "RAM暂存数据逐字节读回核对，Flash仅确认命令执行完成，未完整回读Flash。"
)


class DownloadError(RuntimeError):
    pass


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def frame(body, crc):
    if crc not in ("none", "modbus"):
        raise ValueError("CRC must be none or modbus")
    data = bytes(body)
    if crc == "modbus":
        data += struct.pack("<H", crc16(data))
    if not 1 <= len(data) <= 255:
        raise ValueError("Invalid frame length")
    return b"\x5a\xa5" + bytes([len(data)]) + data


class FrameReader:
    def __init__(self, crc):
        self.crc = crc
        self.buffer = bytearray()
        self.bad_frames = 0

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        while True:
            start = self.buffer.find(b"\x5a\xa5")
            if start < 0:
                self.buffer[:] = b"\x5a" if self.buffer[-1:] == b"\x5a" else b""
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 3:
                break
            length = self.buffer[2]
            if length < (5 if self.crc == "modbus" else 3):
                self.bad_frames += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < length + 3:
                break
            packet = bytes(self.buffer[3:length + 3])
            if self.crc == "modbus":
                if crc16(packet[:-2]) != int.from_bytes(packet[-2:], "little"):
                    self.bad_frames += 1
                    del self.buffer[0]
                    continue
                packet = packet[:-2]
            del self.buffer[:length + 3]
            frames.append(packet)
        return frames


class Client:
    """Serial transport is injected. 82 ACK is optional; 83 response is required."""

    def __init__(self, port, crc, timeout=1.5):
        self.port = port
        self.crc = crc
        self.timeout = timeout
        self.reader = FrameReader(crc)

    def send(self, body):
        packet = frame(body, self.crc)
        if self.port.write(packet) != len(packet):
            raise DownloadError("串口未完整发送数据帧")
        self.port.flush()

    def write(self, vp, data):
        data = bytes(data)
        if not data or len(data) > CHUNK_SIZE or len(data) % 2:
            raise ValueError("Write data must contain 1..120 words")
        if vp < 0 or vp + len(data) // 2 > 0x10000:
            raise ValueError("VP write out of range")
        self.send(b"\x82" + struct.pack(">H", vp) + data)

    def read(self, vp, words):
        if not 1 <= words <= CHUNK_SIZE // 2 or vp < 0 or vp + words > 0x10000:
            raise ValueError("VP read out of range")
        # No outstanding read request is allowed. Remove old ACKs/responses first.
        self.port.reset_input_buffer()
        self.reader.buffer.clear()
        self.send(b"\x83" + struct.pack(">HB", vp, words))
        expected = b"\x83" + struct.pack(">HB", vp, words)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            for body in self.reader.feed(self.port.read(256)):
                if body.startswith(expected) and len(body) == 4 + words * 2:
                    return body[4:]
        raise DownloadError(f"读取VP {vp:04X} 超时/未收到有效应答")

    def write_check(self, vp, data):
        self.write(vp, data)
        if self.read(vp, len(data) // 2) != bytes(data):
            raise DownloadError(f"VP {vp:04X} 写入读回不一致")


def identify(client):
    version = client.read(0x000F, 1)
    if version in (b"\x00\x00", b"\xff\xff") or not all(version):
        raise DownloadError("版本应答无有效GUI/OS版本")
    native_size = struct.unpack(">HH", client.read(0x007A, 2))
    config = client.read(0x0080, 2)
    rotation = config[3] & 0x03
    # LCD_HOR/VER describe the panel; D0[1:0] selects the displayed orientation.
    size = native_size[::-1] if rotation % 2 else native_size
    if size != (800, 480):
        raise DownloadError(f"屏幕原生分辨率{native_size}、旋转{rotation * 90}°与HT的800×480不匹配")
    return {
        "crc": client.crc,
        "gui_version_hex": f"{version[0]:02X}",
        "os_version_hex": f"{version[1]:02X}",
        "resolution": list(size),
        "native_resolution": list(native_size),
        "rotation_degrees": rotation * 90,
        "config_hex": config.hex(),
        "page": int.from_bytes(client.read(0x0014, 1), "big"),
    }


def probe(port, crc="auto", timeout=1.5):
    errors = []
    for mode in ("none", "modbus") if crc == "auto" else (crc,):
        try:
            return identify(Client(port, mode, timeout))
        except DownloadError as error:
            errors.append(f"{mode}: {error}")
    raise DownloadError("；".join(errors))


def make_plan(directory=DEFAULT_SET, keep_font=False):
    directory = Path(directory).resolve()
    files = []
    for name, file_id, slots in RESOURCES:
        path = directory / name
        if not path.is_file() or path.is_symlink():
            raise DownloadError(f"缺少普通资源文件：{path}")
        data = path.read_bytes()
        if not data or len(data) > slots * SLOT_SIZE:
            raise DownloadError(f"文件为空或越过分配范围：{name}")
        if name.endswith(".icl") and not data.startswith(b"DGUS_3"):
            raise DownloadError(f"不是当前DGUS_3资源：{name}")
        if name == "14ShowFile.bin" and not data.startswith(b"\x14DGUS_2"):
            raise DownloadError("14文件格式与本项目不符")
        if name == "22_Config.bin" and len(data) != 0x20004:
            raise DownloadError("22文件应为官方导出的128KiB+4字节，禁止自动裁头")
        count = math.ceil(len(data) / BLOCK_SIZE)
        start = file_id * SLOT_SIZE
        end = start + count * BLOCK_SIZE
        if end > 16 * 1024 * 1024:
            raise DownloadError(f"Flash范围越界：{name}")
        files.append({
            "name": name, "path": str(path), "id": file_id,
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "block_start": file_id * 8, "block_count": count,
            "flash_start": start, "flash_end_exclusive": end,
            "tail_ff_bytes": count * BLOCK_SIZE - len(data),
            "install": not (keep_font and file_id == 0),
        })
    ordered = sorted(files, key=lambda item: item["flash_start"])
    for first, second in zip(ordered, ordered[1:]):
        if first["flash_end_exclusive"] > second["flash_start"]:
            raise DownloadError(f"资源重叠：{first['name']} / {second['name']}")
    return {"directory": str(directory), "files": files, "notice": FLASH_NOTICE,
            "cfg_kernel_os": "unchanged", "ram_vp": RAM_VP,
            "block_size": BLOCK_SIZE, "crc_flash_verified": False}


def blocks(data):
    for offset in range(0, len(data), BLOCK_SIZE):
        yield data[offset:offset + BLOCK_SIZE].ljust(BLOCK_SIZE, b"\xff")


def stage_ram(client, data):
    if len(data) != BLOCK_SIZE:
        raise ValueError("Expected a complete 32KiB block")
    for offset in range(0, BLOCK_SIZE, CHUNK_SIZE):
        client.write(RAM_VP + offset // 2, data[offset:offset + CHUNK_SIZE])
    for offset in range(0, BLOCK_SIZE, CHUNK_SIZE):
        expected = data[offset:offset + CHUNK_SIZE]
        actual = client.read(RAM_VP + offset // 2, len(expected) // 2)
        if actual != expected:
            raise DownloadError(f"暂存RAM读回不一致，块内偏移0x{offset:04X}；未提交Flash")


def prepare_commit(client):
    status = client.read(0x00AA, 6)
    if status[0] == 0x5A:
        raise DownloadError("屏幕已有Flash操作，禁止覆盖busy描述符")
    idle = bytes(12)
    client.write_check(0x00AA, idle)


def require_flash_idle(client):
    if client.read(0x00AA, 6)[0] == 0x5A:
        raise DownloadError("屏幕已有Flash操作，禁止覆盖暂存RAM")


def commit_block(client, block, timeout=10.0):
    if not 0 <= block < 512:
        raise ValueError("32KiB block outside 16MiB flash")
    descriptor = struct.pack(">BBHHHI", 0x5A, 2, block, RAM_VP, 20, 0)
    client.write(0x00AA, descriptor)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.read(0x00AA, 6)
        if status[1:] != descriptor[1:]:
            raise DownloadError("Flash描述符不匹配/写命令未被接收；停止下载")
        if status[0] == 0:
            return
        if status[0] != 0x5A:
            raise DownloadError("Flash返回未知操作状态；停止下载")
        time.sleep(0.02)
    raise DownloadError("Flash busy超时；不自动重启屏幕")


def utc_now():
    return datetime.now(timezone.utc).isoformat()


class Journal:
    def __init__(self, path, plan, port, crc):
        self.path = Path(path).resolve()
        if not self.path.is_relative_to((ROOT / ".tools").resolve()):
            raise DownloadError("进度JSON必须放在本仓库.tools内")
        if self.path.suffix.lower() != ".json" or self.path.exists():
            raise DownloadError("进度路径必须是尚不存在的.json文件，避免覆盖历史记录")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.data = {"started": utc_now(), "port": port, "crc": crc,
                     "plan": plan, "stage": "planned", "blocks": [],
                     "flash_content_readback": False}
        self.save()

    def save(self):
        self.data["updated"] = utc_now()
        temp = self.path.with_suffix(".json.tmp")
        temp.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf-8")
        temp.replace(self.path)

    def stage(self, stage, **fields):
        self.data.update(stage=stage, **fields)
        self.save()


def install(client, plan, journal, announce=print):
    try:
        # Read and hash every allowed input before the first screen mutation.
        payloads = []
        for item in plan["files"]:
            if not item["install"]:
                continue
            data = Path(item["path"]).read_bytes()
            if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise DownloadError(f"规划后源文件变化：{item['name']}")
            payloads.append((item, data))
        before = identify(client)
        journal.stage("identified", before=before)
        require_flash_idle(client)
        # 55AA5AA5 stops GUI refresh only. 55AA5AAA would stop OS and is forbidden.
        client.write(0x00FC, bytes.fromhex("55AA5AA5"))
        client.read(0x000F, 1)
        journal.stage("gui_stop_sent")
        for item, data in payloads:
            for index, chunk in enumerate(blocks(data)):
                block = item["block_start"] + index
                record = {"file": item["name"], "block": block, "stage": "staging_ram",
                          "sha256": hashlib.sha256(chunk).hexdigest()}
                journal.data["blocks"].append(record)
                journal.stage("installing")
                require_flash_idle(client)
                stage_ram(client, chunk)
                record["stage"] = "ram_verified"
                journal.save()
                prepare_commit(client)
                # A failure after this checkpoint leaves Flash completion unknown.
                record["stage"] = "flash_started"
                journal.save()
                commit_block(client, block)
                record["stage"] = "flash_command_completed"
                journal.save()
                announce(f"{item['name']} {index + 1}/{item['block_count']}：RAM核对一致，Flash块{block:04X}操作完成")
        journal.stage("reset_pending")
        client.write(0x0004, bytes.fromhex("55AA5AA5"))
        time.sleep(1.0)
        after = identify(client)
        journal.stage("restarted", after=after)
        client.write_check(0x00DE, bytes.fromhex("5A000020"))
        client.write(0x0084, bytes.fromhex("5A010000"))
        deadline = time.monotonic() + 3.0
        while True:
            page = int.from_bytes(client.read(0x0014, 1), "big")
            if page == 0:
                break
            if time.monotonic() >= deadline:
                raise DownloadError(f"页面0切换未确认，当前页{page}")
            time.sleep(0.05)
        journal.stage("complete", displayed_page=page, notice=FLASH_NOTICE)
        return journal.data
    except (Exception, KeyboardInterrupt) as error:
        journal.stage("failed", error=str(error) or type(error).__name__, automatic_reset_on_failure=False)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("plan", "install"):
        action = commands.add_parser(command)
        action.add_argument("--set-dir", type=Path, default=DEFAULT_SET)
        action.add_argument("--keep-font", action="store_true", help="保留屏幕出厂0号ASCII字库")
        if command == "install":
            action.add_argument("--port", required=True)
            action.add_argument("--crc", choices=("none", "modbus"), required=True)
            action.add_argument("--journal", type=Path, required=True)
    action = commands.add_parser("probe")
    action.add_argument("--port", required=True)
    action.add_argument("--crc", choices=("auto", "none", "modbus"), default="auto")
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            print(json.dumps(make_plan(args.set_dir, args.keep_font), ensure_ascii=False, indent=2))
            return 0
        plan = make_plan(args.set_dir, args.keep_font) if args.command == "install" else None
        journal = Journal(args.journal, plan, args.port, args.crc) if plan else None
        try:
            import serial  # plan and unit tests need no serial package or hardware.
            port = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=2)
            port.dtr = False
            port.rts = False
            port.port = args.port
            with port:
                if args.command == "probe":
                    result = probe(port, args.crc)
                else:
                    result = install(Client(port, args.crc), plan, journal)
                print(json.dumps(result, ensure_ascii=False, indent=2))
        except Exception as error:
            if journal and journal.data["stage"] != "failed":
                journal.stage("failed", error=str(error), automatic_reset_on_failure=False)
            raise
        return 0
    except Exception as error:
        print(f"失败：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
