"""生成 HT 状态、设置和日志页，以及原生图标和布局/VP 契约。"""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 2
COLORS = dict(bg="#FFFFFF", surface="#FFFFFF", ink="#303133", muted="#606266",
              line="#DCDFE6", divider="#EBEEF5", accent="#409EFF", pale="#ECF5FF",
              red="#F56C6C", red_pale="#FEF0F0", amber="#E6A23C", amber_pale="#FDF6EC")
FONT_DIR = Path("C:/Windows/Fonts")
EVENTS = ["", "启动完成", "屏幕连接", "屏幕断开", "阻抗请求确认", "频率请求确认",
          "电池正常", "电池低电或异常", "过流保护触发", "过流保护解除", "过压保护触发",
          "过压保护解除", "温度就绪", "温度无效", "温度采样未就绪", "输入输出错误"]


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


def header(art, page):
    art.text((24, 18), "HT", 28, COLORS["accent"], True)
    art.text((78, 25), "音频信号源", 18, COLORS["ink"])
    for index, title in enumerate(["状态", "设置", "日志"]):
        x = 300 + 102 * index
        selected = index == page
        art.text((x + 22, 25), title, 18, COLORS["accent"] if selected else COLORS["muted"], selected)
        if selected:
            art.rect((x + 12, 60, x + 70, 63), COLORS["accent"])
    art.line([(24, 64), (776, 64)], COLORS["divider"])


def footer(art, label):
    art.line([(24, 446), (776, 446)], COLORS["divider"])
    art.text((24, 458), label, 14, COLORS["muted"])


def status_page():
    art = Art(800, 480, COLORS["bg"])
    header(art, 0)
    for x, title, unit in [(24, "当前电流", "A"), (408, "当前电压", "V")]:
        art.rect((x, 80, x + 368, 220), COLORS["surface"], 4, COLORS["line"])
        art.text((x + 24, 96), title, 20, COLORS["muted"])
        art.text((x + 328, 166), unit, 24, COLORS["muted"], mono=True)
    for x, title, unit in [(24, "当前阻抗挡位", "Ω"), (408, "当前频率挡位", "kHz")]:
        art.rect((x, 232, x + 368, 294), COLORS["surface"], 4, COLORS["line"])
        art.text((x + 16, 254), title, 18, COLORS["muted"])
        art.text((x + 300, 258), unit, 20, COLORS["muted"], mono=unit == "kHz")
    for index in range(3):
        x = 24 + 256 * index
        art.rect((x, 308, x + 240, 390), COLORS["surface"], 4, COLORS["line"])
        art.text((x + 16, 322), f"NTC{index + 1}", 16, COLORS["muted"], mono=True)
        art.text((x + 198, 357), "℃", 18, COLORS["muted"])
    art.text((40, 413), "运行时间", 18, COLORS["muted"])
    footer(art, "← / → 切换页面")
    return art


def settings_page():
    art = Art(800, 480, COLORS["bg"])
    header(art, 1)
    art.text((24, 90), "确认保存请求，输出仍关闭", 18, COLORS["muted"])
    art.text((24, 414), "离开设置页将放弃未确认的修改", 16, COLORS["muted"])
    footer(art, "← / → 切页    ↑ / ↓ 选项    旋钮循环预选    OK / 下压确认")
    return art


def logs_page():
    art = Art(800, 480, COLORS["bg"])
    header(art, 2)
    art.text((24, 88), "设备日志", 22, COLORS["ink"], True)
    for x, label in [(40, "上电时间"), (238, "事件"), (626, "数值")]:
        art.text((x, 124), label, 16, COLORS["muted"])
    for index in range(4):
        y = 146 + 60 * index
        art.line([(24, y + 54), (776, y + 54)], COLORS["divider"])
    art.text((40, 410), "仅记录本次上电后的事件", 16, COLORS["muted"])
    footer(art, "← / → 切页    ↑ / ↓ 或旋钮翻看    OK / 下压返回最新")
    return art


def state_icon(label, color, background):
    art = Art(136, 32, COLORS["bg"])
    art.rect((0, 0, 135, 31), background, 4)
    art.rect((14, 13, 20, 19), color, 3)
    art.text((32, 7), label, 18, color, True)
    return art


def battery_icon(label, color):
    art = Art(208, 28, COLORS["bg"])
    art.rect((2, 6, 25, 21), None, 2, color, 2)
    art.rect((26, 10, 29, 17), color, 1)
    art.text((40, 5), label, 16, color)
    return art


def focus_icon(selected):
    art = Art(752, 200, COLORS["bg"])
    for index, (label, unit) in enumerate([("阻抗挡位", "Ω"), ("频率挡位", "kHz")]):
        top = index * 112
        focused = selected == index
        color = COLORS["accent"] if focused else COLORS["ink"]
        art.rect((0, top, 751, top + 87), COLORS["pale"] if focused else COLORS["surface"],
                 4, COLORS["accent"] if focused else COLORS["line"])
        art.text((24, top + 28), label, 26, color, focused)
        art.text((664, top + 36), unit, 20, COLORS["muted"], mono=unit == "kHz")
    return art


def edit_icon(pending):
    art = Art(752, 44, COLORS["bg"])
    label = "待确认 · 按 OK 或编码器保存请求" if pending else "请求已保存 / 无修改"
    art.text((0, 12), label, 20, COLORS["accent"] if pending else COLORS["muted"], pending)
    return art


def event_icon(index, label):
    art = Art(380, 50, COLORS["bg"])
    if index:
        color = COLORS["red"] if index in (7, 8, 10, 15) else (
            COLORS["amber"] if index in (3, 13, 14) else COLORS["accent"])
        art.rect((8, 22, 14, 28), color, 3)
        art.text((30, 15), label, 20, COLORS["ink"])
    return art


def text_field(name, label, vp, page, x, y, width, height, size, unit="", decimals=0, scale=1):
    return dict(name=name, label=label, unit=unit, vp=vp, words=16,
                encoding="ascii", display_encoding="gbk", input_scale=scale, decimals=decimals,
                x=x, y=y, width=width, height=height, font_width=size // 2, font_height=size,
                max_chars=9, color=COLORS["ink"], pages=[page])


def build():
    for index, page in enumerate([status_page(), settings_page(), logs_page()]):
        page.save(ROOT / f"assets/pages/{index:03d}.png")
    icons = [state_icon(label, color, background) for label, color, background in [
        ("待机", COLORS["muted"], "#F4F4F5"),
        ("运行中", COLORS["accent"], COLORS["pale"]),
        ("切档中", COLORS["amber"], COLORS["amber_pale"]),
        ("保护", COLORS["red"], COLORS["red_pale"]),
        ("未连接", COLORS["muted"], "#F4F4F5"),
    ]]
    icons += [battery_icon(label, color) for label, color in [
        ("电池正常", COLORS["muted"]), ("电池低电 / 异常", COLORS["red"]), ("电池未知", COLORS["muted"]),
    ]]
    icons += [focus_icon(index) for index in range(2)]
    icons += [edit_icon(False), edit_icon(True)]
    icons += [event_icon(index, label) for index, label in enumerate(EVENTS)]
    for index, icon in enumerate(icons):
        icon.save(ROOT / f"assets/icons/{index:03d}.png")

    fields = []
    specs = [
        ("current", "当前电流", "A", 3, 1000, 48, 128, 288, 72, 64, 0),
        ("voltage", "当前电压", "V", 3, 1000, 432, 128, 288, 72, 64, 0),
        ("range", "当前阻抗挡位", "ohm", 0, 1, 200, 246, 112, 36, 32, 0),
        ("frequency", "当前频率挡位", "kHz", 0, 1000, 600, 246, 88, 36, 32, 0),
        ("elapsed", "运行时间", "HHH:MM:SS", 0, 1, 146, 407, 144, 32, 28, 0),
        ("range_choice", "预选阻抗挡位", "ohm", 0, 1, 458, 150, 216, 44, 40, 1),
        ("frequency_choice", "预选频率挡位", "kHz", 0, 1000, 458, 262, 216, 44, 40, 1),
    ]
    for index, (name, label, unit, decimals, scale, x, y, width, height, size, page) in enumerate(specs):
        fields.append(text_field(name, label, 0x1100 + index * 0x10, page, x, y, width, height, size,
                                 unit, decimals, scale))
        if name in ("range", "frequency"):
            fields[-1]["max_chars"] = 4 if name == "range" else 2
    for index in range(3):
        fields.append(text_field(f"ntc{index + 1}", f"NTC{index + 1}", 0x1170 + index * 0x10,
                                 0, 40 + index * 256, 346, 160, 36, 32, "degC", 1, 10))
    for index in range(4):
        y = 157 + index * 60
        fields.append(text_field(f"log_time_{index}", "事件时间", 0x1200 + index * 0x20,
                                 2, 40, y, 126, 32, 28, "HHH:MM:SS"))
        fields.append(text_field(f"log_value_{index}", "事件数值", 0x1210 + index * 0x20,
                                 2, 626, y, 126, 32, 28))
    fields.append(text_field("log_position", "日志位置", 0x1280, 2, 626, 402, 126, 32, 28))
    variable_icons = [
        dict(name="state", vp=0x1000, x=640, y=20, width=136, height=32, first=0, last=4,
             initial=4, values=["standby", "running", "switching", "fault", "offline"], pages=[0, 1, 2]),
        dict(name="battery", vp=0x1001, x=568, y=408, width=208, height=28, first=5, last=7,
             initial=2, values=["normal", "low_or_abnormal", "unknown"], pages=[0]),
        dict(name="focus", vp=0x1002, x=24, y=128, width=752, height=200, first=8, last=9,
             initial=0, values=["range", "frequency"], pages=[1]),
        dict(name="edit", vp=0x1003, x=24, y=354, width=752, height=44, first=10, last=11,
             initial=0, values=["confirmed", "pending"], pages=[1]),
    ]
    for index in range(4):
        variable_icons.append(dict(name=f"log_event_{index}", vp=0x1010 + index,
                                   x=208, y=146 + index * 60, width=380, height=50,
                                   first=12, last=27, initial=0, values=EVENTS, pages=[2]))
    spec = dict(version=3, name="HT", width=800, height=480, touch=False, colors=COLORS,
                page_register=0x0084, background_library=32, icon_library=42,
                pages=[dict(id=index, name=name, image=f"assets/pages/{index:03d}.png")
                       for index, name in enumerate(["status", "settings", "logs"])],
                fields=fields, icons=variable_icons, animations=[],
                limits=dict(text_bytes=32, text_max_chars=9, render_rate_hz=5))
    (ROOT / "ui.json").write_text(json.dumps(spec, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated 3 pages, {len(icons)} icons and ui.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--font-dir", type=Path, default=FONT_DIR)
    args = parser.parse_args()
    FONT_DIR = args.font_dir
    build()
