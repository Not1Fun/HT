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
EVENTS = ["", "启动完成", "屏幕连接", "屏幕断开", "阻抗匹配", "频率请求确认",
          "电池正常", "电池低电或异常", "OC 保护输入有效", "OC 保护输入解除", "OV 保护输入有效",
          "OV 保护输入解除", "温度就绪", "温度无效", "温度采样未就绪", "输入输出错误",
          "输出启动", "输出停止", "输出故障"]

# 顺序对应 firmware/src/core/output_reason.h；末尾为 POWER_ERROR_IO..MATCH。
REASONS = ["", "输出模块未初始化，检查启动日志", "输出服务已关闭，需要重新上电",
    "面板或主板输入读取失败", "面板输入更新超时", "屏幕通信未连接",
    "使能开关未接通", "机箱门互锁未闭合", "采样板在位信号无效（SNS_PRESENT）",
    "DC_OK 未就绪，检查电源状态线", "安全继电器线圈回路未闭合",
    "OC 保护输入为低，需检查检测板或锁存状态",
    "OV 保护输入为低，需检查检测板或锁存状态",
    "交流供电和电池正常信号均无效", "ADC / DAC 采样外设异常",
    "电压电流采样无效或更新超时", "温度采样更新超时",
    "NTC1 开路，整机输出需要接好探头", "NTC2 开路，整机输出需要接好探头",
    "NTC3 开路，整机输出需要接好探头", "NTC1 短路或测量无效",
    "NTC2 短路或测量无效", "NTC3 短路或测量无效",
    "NTC1 温度达到 80℃", "NTC2 温度达到 80℃", "NTC3 温度达到 80℃",
    "输出故障已锁存，需排除故障后重新上电", "整机输出忙，请先停止输出",
    "继电器尚未全部释放", "目标功率为 0 VA，请设置并确认",
    "继电器或输出驱动操作失败（故障 1）", "运行期间互锁或保护输入失效（故障 2）",
    "采样外设异常或反馈超时（故障 3）", "电压、电流或功率超过软件限值（故障 4）",
    "检测到负载开路（故障 5）", "检测到负载短路（故障 6）",
    "阻抗匹配失败，未得到有效负载反馈（故障 7）"]


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
    footer(art, "← / → 切换页面    ↓ 立即停止输出")
    return art


def settings_page():
    art = Art(800, 480, COLORS["bg"])
    header(art, 1)
    footer(art, "← / → 切页    ↑ / ↓ 选项    旋钮预选    OK / 下压确认或停止")
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


def debug_page(dac=False):
    art = Art(800, 480, COLORS["bg"])
    header(art, 3)
    art.text((24, 25), "DAC 独立试波" if dac else "BENCH 调试", 20, COLORS["accent"], True)
    art.text((24, 76), "继电器保持释放 · PA4 / U31 观察波形" if dac else "旋钮预选，确认执行；切换前自动静音", 16, COLORS["muted"])
    for x, y, label in [(516, 104, "线圈命令  K8 → K1"), (516, 190, "剩余时间"),
                         (516, 256, "当前电压"), (516, 326, "当前电流")]:
        art.text((x, y), label, 16, COLORS["muted"])
    art.text((744, 217), "s", 20, COLORS["muted"], mono=True)
    art.text((744, 286), "V", 20, COLORS["muted"], mono=True)
    art.text((744, 356), "A", 20, COLORS["muted"], mono=True)
    footer(art, "← / → 离页停止    ↑ / ↓ 选项    旋钮预选    OK / 下压执行")
    return art


def debug_focus(selected, dac=False):
    art = Art(452, 256, COLORS["bg"])
    for index, (label, unit) in enumerate([("挡位继电器" if dac else "继电器", ""), ("试波频率", "kHz"),
                                           ("DAC 幅度", "mVpp"), ("试波开关", "")]):
        top = index * 64
        focused = index == selected
        art.rect((0, top, 451, top + 55), COLORS["pale"] if focused else COLORS["bg"],
                 4, COLORS["accent"] if focused else COLORS["line"])
        art.text((18, top + 17), label, 20, COLORS["accent"] if focused else COLORS["ink"], focused)
        if unit: art.text((374, top + 20), unit, 16, COLORS["muted"], mono=True)
    return art


def debug_status(label, color):
    art = Art(752, 36, COLORS["bg"])
    art.text((0, 8), label, 18, color, True)
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
    art = Art(752, 32, COLORS["bg"])
    label = "待确认 · 按 OK 或编码器执行" if pending else "已确认 / 无修改"
    art.text((0, 6), label, 18, COLORS["accent"] if pending else COLORS["muted"], pending)
    return art


def settings_focus_icon(selected):
    art = Art(752, 228, COLORS["bg"])
    for index, (label, unit) in enumerate([("频率挡位", "kHz"), ("目标视在功率", "VA"), ("输出控制", "")]):
        top = index * 80
        focused = selected == index
        color = COLORS["accent"] if focused else COLORS["ink"]
        art.rect((0, top, 751, top + 65), COLORS["pale"] if focused else COLORS["surface"],
                 4, COLORS["accent"] if focused else COLORS["line"])
        art.text((24, top + 20), label, 22, color, focused)
        if unit:
            art.text((664, top + 22), unit, 20, COLORS["muted"], mono=True)
    return art


def output_icon(label, color):
    art = Art(400, 44, COLORS["bg"])
    art.text((18, 10), label, 20, color, True)
    return art


def event_icon(index, label):
    art = Art(380, 50, COLORS["bg"])
    if index:
        color = COLORS["red"] if index in (7, 8, 10, 15, 18) else (
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
    for index, page in enumerate([status_page(), settings_page(), logs_page(), debug_page(), debug_page(True)]):
        page.save(ROOT / f"assets/pages/{index:03d}.png")
    icons = [state_icon(label, color, background) for label, color, background in [
        ("待机", COLORS["muted"], "#F4F4F5"),
        ("运行中", COLORS["accent"], COLORS["pale"]),
        ("自动匹配", COLORS["amber"], COLORS["amber_pale"]),
        ("保护", COLORS["red"], COLORS["red_pale"]),
        ("未连接", COLORS["muted"], "#F4F4F5"),
    ]]
    icons += [battery_icon(label, color) for label, color in [
        ("电池正常", COLORS["muted"]), ("电池低电 / 异常", COLORS["red"]), ("电池未知", COLORS["muted"]),
    ]]
    icons += [focus_icon(index) for index in range(2)]
    icons += [edit_icon(False), edit_icon(True)]
    icons += [event_icon(index, label) for index, label in enumerate(EVENTS)]
    icons += [settings_focus_icon(index) for index in range(3)]
    icons += [output_icon(label, color) for label, color in [
        ("关闭", COLORS["muted"]), ("准备开启 · 待确认", COLORS["amber"]),
        ("已开启 · 确认停止", COLORS["accent"]), ("禁止启动 · 原因见下方", COLORS["muted"]),
        ("故障 · 输出已停止", COLORS["red"]), ("输出未启用", COLORS["muted"]),
    ]]
    icons += [debug_focus(index) for index in range(4)]
    icons += [debug_status(label, color) for label, color in [
        ("待机 · 继电器最多保持 30 秒，试波最多 10 秒", COLORS["muted"]),
        ("继电器测试 / 切换中 · 离页立即停止", COLORS["amber"]),
        ("正在试波 · 试波开关确认可停止", COLORS["accent"]),
        ("禁止启动 · 具体原因在下方轮流显示", COLORS["muted"]),
        ("保护已停止 · 详情见日志", COLORS["red"]),
    ]]
    icons += [debug_focus(index, True) for index in range(1, 4)]
    icons += [debug_status(label, color) for label, color in [
        ("可试波 · 最长 10 秒；幅度为 DAC 标称峰峰值", COLORS["muted"]),
        ("准备试波 · 继电器释放中", COLORS["amber"]),
        ("正在试波 · 开关项确认或离页立即停止", COLORS["accent"]),
        ("禁止启动 · 具体原因在下方轮流显示", COLORS["muted"]),
        ("保护已停止 · 详情见日志", COLORS["red"]),
    ]]
    for label in REASONS:
        art = Art(752, 28, COLORS["bg"])
        if label: art.text((0, 4), label, 17, COLORS["red"], True)
        icons.append(art)
    for index, icon in enumerate(icons):
        icon.save(ROOT / f"assets/icons/{index:03d}.png")

    fields = []
    specs = [
        ("current", "当前电流", 0x1100, "A", 3, 1000, 48, 128, 288, 72, 64, 0),
        ("voltage", "当前电压", 0x1110, "V", 3, 1000, 432, 128, 288, 72, 64, 0),
        ("range", "当前阻抗挡位", 0x1120, "ohm", 0, 1, 200, 246, 112, 36, 32, 0),
        ("frequency", "当前频率挡位", 0x1130, "kHz", 0, 1000, 600, 246, 88, 36, 32, 0),
        ("elapsed", "运行时间", 0x1140, "HHH:MM:SS", 0, 1, 146, 407, 144, 32, 28, 0),
        ("frequency_choice", "预选频率挡位", 0x1160, "kHz", 0, 1000, 458, 162, 216, 36, 32, 1),
    ]
    for name, label, vp, unit, decimals, scale, x, y, width, height, size, page in specs:
        fields.append(text_field(name, label, vp, page, x, y, width, height, size,
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
    fields.append(text_field("power_choice", "目标视在功率", 0x11a0, 1, 458, 242, 216, 36, 32, "VA", 0, 1000))
    debug_fields = [
        ("debug_relay", "继电器预选", 278, 112, 168, ""),
        ("debug_frequency", "试波频率预选", 278, 176, 112, "kHz"),
        ("debug_amplitude", "DAC幅度预选", 278, 240, 112, "mVpp"),
        ("debug_wave", "试波开关", 278, 304, 168, ""),
        ("debug_coils", "线圈命令K8到K1", 516, 136, 240, ""),
        ("debug_seconds", "剩余时间", 600, 212, 140, "s"),
        ("debug_voltage", "当前电压", 516, 282, 216, "V"),
        ("debug_current", "当前电流", 516, 352, 216, "A"),
    ]
    for index, (name, label, x, y, width, unit) in enumerate(debug_fields):
        item = text_field(name, label, 0x1300 + index * 0x10, 3, x, y, width, 32, 28, unit)
        item["max_chars"] = min(9, width // 14)
        item["pages"] = [3, 4]
        fields.append(item)
    variable_icons = [
        dict(name="state", vp=0x1000, x=640, y=20, width=136, height=32, first=0, last=4,
             initial=4, values=["standby", "running", "switching", "fault", "offline"], pages=[0, 1, 2]),
        dict(name="battery", vp=0x1001, x=568, y=408, width=208, height=28, first=5, last=7,
             initial=2, values=["normal", "low_or_abnormal", "unknown"], pages=[0]),
        dict(name="focus", vp=0x1002, x=24, y=146, width=752, height=228, first=31, last=33,
             initial=0, values=["frequency", "power", "output"], pages=[1]),
        dict(name="edit", vp=0x1003, x=24, y=382, width=752, height=32, first=10, last=11,
             initial=0, values=["confirmed", "pending"], pages=[1]),
        dict(name="output", vp=0x1004, x=346, y=316, width=400, height=44, first=34, last=39,
             initial=5, values=["off", "pending", "running", "unavailable", "fault", "disabled"], pages=[1]),
    ]
    for index in range(4):
        variable_icons.append(dict(name=f"log_event_{index}", vp=0x1010 + index,
                                   x=208, y=146 + index * 60, width=380, height=50,
                                   first=12, last=30, initial=0, values=EVENTS, pages=[2]))
    variable_icons += [
        dict(name="debug_focus", vp=0x1020, x=24, y=100, width=452, height=256,
             first=40, last=43, initial=0, values=["relay", "frequency", "amplitude", "wave"], pages=[3]),
        dict(name="debug_state", vp=0x1021, x=24, y=380, width=752, height=36,
             first=44, last=48, initial=3, values=["idle", "relay", "wave", "blocked", "fault"], pages=[3]),
        dict(name="dac_focus", vp=0x1030, x=24, y=100, width=452, height=256,
             first=49, last=51, initial=0, values=["frequency", "amplitude", "wave"], pages=[4]),
        dict(name="dac_state", vp=0x1031, x=24, y=380, width=752, height=36,
             first=52, last=56, initial=3, values=["idle", "preparing", "wave", "blocked", "fault"], pages=[4]),
    ]
    variable_icons.append(dict(name="reason", vp=0x1005, x=24, y=416, width=752, height=28,
        first=57, last=57 + len(REASONS) - 1, initial=1, values=REASONS, pages=[1, 3, 4]))
    spec = dict(version=9, name="HT", width=800, height=480, touch=False, colors=COLORS,
                page_register=0x0084, background_library=32, icon_library=42,
                pages=[dict(id=index, name=name, image=f"assets/pages/{index:03d}.png")
                       for index, name in enumerate(["status", "settings", "logs", "debug", "dac"])],
                fields=fields, icons=variable_icons, animations=[],
                limits=dict(text_bytes=32, text_max_chars=9, render_rate_hz=5))
    (ROOT / "ui.json").write_text(json.dumps(spec, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated 5 pages, {len(icons)} icons and ui.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--font-dir", type=Path, default=FONT_DIR)
    args = parser.parse_args()
    FONT_DIR = args.font_dir
    build()
