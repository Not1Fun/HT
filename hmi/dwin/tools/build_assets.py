"""生成 HT 800×480 页面、原生图标帧与唯一布局/VP 契约。"""
import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 2
COLORS = dict(bg="#F4F3EF", surface="#FFFFFF", ink="#243331", muted="#727D78",
              line="#DEDfD8", accent="#276A58", pale="#E8F0EB", dark="#253632",
              red="#AF4339", red_pale="#F8EAE5", amber="#8B6628")
FONT_DIR = Path("C:/Windows/Fonts")


def font(size, bold=False, mono=False):
    name = "consolab.ttf" if mono and bold else "consola.ttf" if mono else "msyhbd.ttc" if bold else "msyh.ttc"
    return ImageFont.truetype(str(FONT_DIR / name), size * SCALE)


class Art:
    def __init__(self, width, height, background):
        self.image = Image.new("RGB", (width * SCALE, height * SCALE), background)
        self.draw = ImageDraw.Draw(self.image)

    def rect(self, box, fill, radius=0, outline=None, width=1):
        box = tuple(round(x * SCALE) for x in box)
        if radius:
            self.draw.rounded_rectangle(box, radius * SCALE, fill, outline, width * SCALE)
        else:
            self.draw.rectangle(box, fill, outline, width * SCALE)

    def text(self, xy, value, size=16, color=None, bold=False, mono=False):
        self.draw.text(tuple(x * SCALE for x in xy), value, font=font(size, bold, mono),
                       fill=color or COLORS["ink"], anchor="lt")

    def line(self, points, fill, width=1):
        self.draw.line([(round(x * SCALE), round(y * SCALE)) for x, y in points], fill, width * SCALE)

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.image.resize((self.image.width // SCALE, self.image.height // SCALE), Image.Resampling.LANCZOS).save(path)


def header(art, caption):
    art.rect((0, 0, 800, 64), COLORS["dark"])
    art.text((24, 15), "HT", 30, "#FFFFFF", True)
    art.line([(78, 18), (78, 46)], "#63726A")
    art.text((96, 17), "音频信号源", 18, "#FFFFFF", True)
    art.text((96, 41), caption, 10, "#B5C7BB", mono=True)


def main_page():
    a = Art(800, 480, COLORS["bg"])
    header(a, "IMPEDANCE MEASUREMENT")
    a.rect((24, 86, 448, 318), COLORS["surface"], 8)
    a.rect((464, 86, 776, 318), COLORS["surface"], 8)
    a.text((44, 105), "阻抗  |Z|", 18, COLORS["muted"])
    a.text((395, 172), "Ω", 26, COLORS["muted"])
    a.line([(44, 226), (428, 226)], COLORS["line"])
    a.text((44, 242), "R", 16, COLORS["muted"], mono=True)
    a.text((238, 242), "X", 16, COLORS["muted"], mono=True)
    a.text((196, 249), "Ω", 14, COLORS["muted"])
    a.text((406, 249), "Ω", 14, COLORS["muted"])
    a.text((44, 285), "相位", 14, COLORS["muted"])
    a.text((248, 285), "°", 18, COLORS["muted"])
    a.line([(620, 104), (620, 298)], COLORS["line"])
    a.line([(484, 199), (756, 199)], COLORS["line"])
    for x, y, label, unit in [(484, 106, "电压", "V"), (640, 106, "电流", "A"),
                              (484, 218, "视在功率", "VA"), (640, 218, "有功功率", "W")]:
        a.text((x, y), label, 14, COLORS["muted"])
        a.text((x, y + 70), unit, 13, COLORS["muted"], mono=True)
    a.line([(24, 428), (776, 428)], COLORS["line"])
    for x, label in [(24, "T1"), (151, "T2"), (278, "T3"), (405, "母线")]:
        a.text((x, 448), label, 12, COLORS["muted"], mono=label.startswith("T"))
    for x in [119, 246, 373]:
        a.text((x, 448), "°C", 12, COLORS["muted"])
    a.text((524, 448), "V", 12, COLORS["muted"], mono=True)
    return a


def boot_page():
    a = Art(800, 480, COLORS["bg"])
    header(a, "STARTUP CHECK")
    a.text((65, 118), "HT", 76, COLORS["ink"], True)
    a.text((68, 207), "音频信号源 · 阻抗测量", 22, COLORS["ink"])
    a.text((69, 246), "2–10 kHz  /  CC · VA  /  7 档量程", 15, COLORS["muted"])
    a.line([(69, 292), (731, 292)], COLORS["line"])
    a.text((70, 319), "启动检查", 14, COLORS["muted"])
    a.text((70, 426), "使能开关保持断开，检查完成后再开始测量。", 16, COLORS["muted"])
    return a


def fault_page():
    a = Art(800, 480, COLORS["bg"])
    header(a, "PROTECTION")
    a.rect((24, 88, 776, 397), COLORS["surface"], 8)
    a.rect((24, 88, 31, 397), COLORS["red"])
    a.rect((56, 120, 112, 176), COLORS["red_pale"], 6)
    a.text((77, 126), "!", 36, COLORS["red"], True, True)
    a.text((136, 122), "保护已触发", 32, COLORS["red"], True)
    a.text((137, 167), "当前测量无效", 15, COLORS["muted"])
    a.line([(56, 288), (744, 288)], COLORS["line"])
    a.text((56, 316), "01   断开使能开关，检查负载与接线", 20, COLORS["ink"])
    a.text((56, 356), "02   排除故障后重新使能，不会自动恢复输出", 17, COLORS["muted"])
    a.text((25, 444), "请以实际输出与硬件保护状态为准。", 15, COLORS["muted"])
    return a


def label_icon(width, height, label, color, background, size=16):
    a = Art(width, height, background)
    a.text((4, max(1, (height - size) // 2 - 1)), label, size, color, True)
    return a


def selector(index):
    a = Art(752, 76, COLORS["bg"])
    for i, title in enumerate(["频率", "目标值", "当前量程"]):
        x = i * 256
        selected = i == index
        a.rect((x, 0, x + 239, 75), COLORS["pale"] if selected else COLORS["surface"], 6,
               COLORS["accent"] if selected else COLORS["line"], 2 if selected else 1)
        a.text((x + 18, 10), title, 13, COLORS["accent"] if selected else COLORS["muted"])
        if selected:
            a.rect((x + 217, 12, x + 223, 18), COLORS["accent"], 2)
    a.text((97, 12), "实际", 11, COLORS["muted"])
    a.text((189, 42), "Hz", 15, COLORS["muted"], mono=True)
    a.text((700, 42), "Ω", 16, COLORS["muted"])
    return a


def ring(size, frame, background, active=True):
    a = Art(size, size, background)
    center = size / 2
    for i in range(12):
        angle = 2 * math.pi * i / 12 - math.pi / 2
        fade = ((i - frame) % 12) / 11 if active else 0
        base = (101, 153, 123) if background == COLORS["dark"] else (39, 106, 88)
        bg = tuple(int(background[j:j+2], 16) for j in (1, 3, 5))
        alpha = 0.24 + 0.76 * fade if active else 0.22
        fill = tuple(round(bg[j] + (base[j] - bg[j]) * alpha) for j in range(3))
        r0, r1 = size * .27, size * .40
        a.line([(center + math.cos(angle)*r0, center + math.sin(angle)*r0),
                (center + math.cos(angle)*r1, center + math.sin(angle)*r1)], fill, max(2, size // 16))
    return a


def build():
    (ROOT / "assets/pages").mkdir(parents=True, exist_ok=True)
    for i, page in enumerate([boot_page(), main_page(), fault_page()]):
        page.save(ROOT / f"assets/pages/{i:03d}.png")
    icons = [selector(i) for i in range(3)]
    icons += [label_icon(120, 28, s, "#DCEADD", COLORS["dark"], 17) for s in ["恒流 CC", "恒功率 VA"]]
    icons += [label_icon(60, 20, s, COLORS["accent"], COLORS["pale"], 12) for s in ["手动", "自动"]]
    icons += [label_icon(126, 28, s, c, COLORS["dark"], 17) for s, c in [
        ("待机", "#C3D0C7"), ("输出中", "#ACD9B6"), ("切档中", "#DFC893"),
        ("保护", "#F0B4A7"), ("数据过期", "#DFC893")]]
    icons += [label_icon(688, 66, s, COLORS["red"], COLORS["surface"], 26) for s in [
        "故障原因待确认", "过流保护", "过压保护", "温度超限", "采样传感器异常", "电源状态异常", "通信异常"]]
    icons += [label_icon(106, 24, "电池 " + s, c, COLORS["bg"], 12) for s, c in [
        ("正常", COLORS["accent"]), ("报警", COLORS["red"]), ("未知", COLORS["muted"])]]
    icons += [label_icon(108, 24, "充电器 " + s, c, COLORS["bg"], 12) for s, c in [
        ("正常", COLORS["accent"]), ("异常", COLORS["red"]), ("未知", COLORS["muted"])]]
    icons += [label_icon(38, 26, s, COLORS["muted"], COLORS["pale"], 16) for s in ["A", "VA"]]
    icons += [ring(32, 0, COLORS["dark"], False)] + [ring(32, i, COLORS["dark"]) for i in range(12)]
    icons += [ring(64, 0, COLORS["bg"], False)] + [ring(64, i, COLORS["bg"]) for i in range(12)]
    icons += [label_icon(126, 24, s, COLORS["muted"], COLORS["surface"], 12) for s in ["未标定", "已标定", "标定待确认"]]
    icons += [label_icon(576, 44, s, COLORS["ink"], COLORS["bg"], 24) for s in [
        "等待主控连接", "正在检查电源与基准", "正在检查保护输入", "正在检查继电器状态", "检查完成，准备就绪", "检查异常，请检查设备"]]
    for i, icon in enumerate(icons):
        icon.save(ROOT / f"assets/icons/{i:03d}.png")

    fields = []
    specs = [
        ("z", "阻抗", "ohm", 3, 1000, 44, 142, 288, 72, 64),
        ("r", "电阻", "ohm", 3, 1000, 64, 240, 126, 30, 28),
        ("x", "电抗", "ohm", 3, 1000, 258, 240, 144, 30, 28),
        ("phase", "相位", "degree", 1, 10, 92, 281, 144, 30, 28),
        ("voltage", "电压", "V", 3, 1000, 484, 139, 132, 34, 28),
        ("current", "电流", "A", 3, 1000, 640, 139, 132, 34, 28),
        ("apparent", "视在功率", "VA", 3, 1000, 484, 251, 132, 34, 28),
        ("power", "有功功率", "W", 3, 1000, 640, 251, 132, 34, 28),
        ("frequency", "实际频率", "Hz", 0, 1, 154, 347, 54, 16, 12),
        ("target", "目标值", "A_or_VA", 3, 1000, 298, 375, 156, 30, 28),
        ("range", "当前量程", "ohm", 3, 1000, 554, 375, 156, 30, 28),
        ("ntc1", "T1", "C", 1, 10, 52, 444, 63, 22, 18),
        ("ntc2", "T2", "C", 1, 10, 179, 444, 63, 22, 18),
        ("ntc3", "T3", "C", 1, 10, 306, 444, 63, 22, 18),
        ("bus", "母线", "V", 3, 1000, 446, 444, 72, 22, 18),
        ("request_frequency", "请求频率", "Hz", 0, 1, 42, 375, 156, 30, 28),
    ]
    for i, (name, label, unit, decimals, scale, x, y, w, h, size) in enumerate(specs):
        fields.append(dict(name=name, label=label, vp=0x1100+i*0x10, words=16,
                           encoding="ascii", input_scale=scale, decimals=decimals,
                           x=x, y=y, width=w, height=h, font_width=size//2, font_height=size,
                           max_chars=min(9, w//(size//2)), color=COLORS["ink"], pages=[1]))
    variable_icons = [
        dict(name="selected", vp=0x1004, x=24, y=335, width=752, height=76, first=0, last=2, pages=[1]),
        dict(name="mode", vp=0x1005, x=330, y=18, width=120, height=28, first=3, last=4, pages=[1]),
        dict(name="auto_range", vp=0x1006, x=656, y=345, width=60, height=20, first=5, last=6, pages=[1]),
        dict(name="state", vp=0x1007, x=598, y=18, width=126, height=28, first=7, last=11, pages=[1, 2]),
        dict(name="fault", vp=0x1008, x=56, y=215, width=688, height=66, first=12, last=18, pages=[2]),
        dict(name="battery", vp=0x1009, x=551, y=442, width=106, height=24, first=19, last=21, pages=[1]),
        dict(name="charger", vp=0x100A, x=669, y=442, width=108, height=24, first=22, last=24, pages=[1]),
        dict(name="target_unit", vp=0x1005, x=464, y=373, width=38, height=26, first=25, last=26, pages=[1]),
        dict(name="calibration", vp=0x100B, x=302, y=104, width=126, height=24, first=53, last=55, pages=[1]),
        dict(name="boot_step", vp=0x100C, x=68, y=354, width=576, height=44, first=56, last=61, pages=[0]),
    ]
    animations = [dict(name="run", vp=0x1000, reserved_vp=0x1001, x=736, y=16, width=32, height=32,
                       stop=27, first=28, last=39, frame_ms=80, pages=[1]),
                  dict(name="boot", vp=0x1002, reserved_vp=0x1003, x=659, y=344, width=64, height=64,
                       stop=40, first=41, last=52, frame_ms=80, pages=[0])]
    spec = dict(version=1, name="HT", width=800, height=480, touch=False, colors=COLORS,
                page_register=0x0084, background_library=32, icon_library=42,
                pages=[dict(id=i, name=n, image=f"assets/pages/{i:03d}.png") for i, n in enumerate(["boot", "measure", "fault"])],
                fields=fields, icons=variable_icons, animations=animations,
                limits=dict(text_bytes=32, text_max_chars=9, render_rate_hz=5),
                animation_warning="原生动画不具备通信超时自停，不代表主控存活或实际功率输出。")
    (ROOT / "ui.json").write_text(json.dumps(spec, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated 3 pages, {len(icons)} icons and ui.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--font-dir", type=Path, default=FONT_DIR)
    args = parser.parse_args()
    FONT_DIR = args.font_dir
    build()
