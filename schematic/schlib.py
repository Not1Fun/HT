# -*- coding: utf-8 -*-
"""
最小可用的 SVG 原理图符号库（IEC 风格）。

坐标约定
--------
- 单位 px，栅格 10px，所有引脚落在栅格上。
- 竖直两端元件：长度 60（pin1 在 (x, y)，pin2 在 (x, y+60)），本体占中间 40。
- 水平两端元件：长度 60（pin1 在 (x, y)，pin2 在 (x+60, y)）。
- 每个符号函数返回引脚坐标字典，方便后续 wire() 连线。

字体栈同时兼顾浏览器（微软雅黑/苹方）和本机 cairosvg 渲染（文泉驿正黑）。
"""

FONT = ("'Microsoft YaHei','PingFang SC','Noto Sans CJK SC',"
        "'WenQuanYi Zen Hei','Source Han Sans SC',sans-serif")
MONO = "'Consolas','DejaVu Sans Mono',monospace" 

LW = 1.8          # 导线线宽
LWS = 1.5         # 符号线宽
COL_WIRE = "#1a1a1a"
COL_SYM = "#1a1a1a"
COL_REF = "#0b5fa5"      # 位号
COL_VAL = "#1a1a1a"      # 参数
COL_NET = "#7a1fa2"      # 网络标号
COL_NOTE = "#b4451a"     # 批注
COL_BOX = "#f7f9fc"      # 模块底色
COL_OFFBRD = "#fff6e8"   # 板外器件底色
COL_DASH = "#2e7d32"     # 板界虚线


class Sheet:
    """一张图纸。"""

    def __init__(self, w, h, no, title, short=None,
                 project="HT 高压电缆接头检测 · 音频源",
                 rev="A", date="2026-09-09"):
        self.w, self.h = w, h
        self.no, self.title = no, title
        self.short = short or title
        self.project, self.rev, self.date = project, rev, date
        self.el = []

    def add(self, s):
        self.el.append(s)
        return self

    # ---------- 基本图元 ----------
    def wire(self, *pts, color=None, width=None, dash=None):
        """折线连接，pts 为 (x, y) 序列。"""
        d = " ".join(f"{'M' if i == 0 else 'L'}{x},{y}" for i, (x, y) in enumerate(pts))
        da = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<path d="{d}" fill="none" stroke="{color or COL_WIRE}" '
                 f'stroke-width="{width or LW}" stroke-linecap="round" '
                 f'stroke-linejoin="round"{da}/>')

    def bus(self, *pts):
        self.wire(*pts, width=4.0)

    def dot(self, x, y, r=4.0):
        """连接点（实心圆）。"""
        self.add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{COL_WIRE}"/>')

    def text(self, x, y, s, size=13, color=COL_VAL, anchor="start",
             weight="normal", font=None, style="normal"):
        s = (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))
        # 中文字体常无独立 bold 字面，用细描边做仿粗体，保证任何渲染器下都不掉字
        extra = (f' stroke="{color}" stroke-width="{size*0.035:.2f}" '
                 f'paint-order="stroke fill"') if weight == "bold" else ""
        self.add(f'<text x="{x}" y="{y}" font-family="{font or FONT}" '
                 f'font-size="{size}" fill="{color}" text-anchor="{anchor}" '
                 f'font-style="{style}"{extra}>{s}</text>')
        return self

    def mtext(self, x, y, lines, size=13, color=COL_VAL, anchor="start",
              lh=None, weight="normal", font=None):
        lh = lh or size + 5
        for i, ln in enumerate(lines):
            self.text(x, y + i * lh, ln, size, color, anchor, weight, font)

    def rect(self, x, y, w, h, fill="none", stroke=COL_SYM, sw=None, rx=0, dash=None):
        da = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" '
                 f'fill="{fill}" stroke="{stroke}" stroke-width="{sw or LWS}"{da}/>')

    # ---------- 标注 ----------
    def rv(self, x, y, ref, val, anchor="start", size=13, gap=17):
        """位号 + 参数两行标注。"""
        if ref:
            self.text(x, y, ref, size, COL_REF, anchor, "bold")
        if val:
            self.text(x, y + (gap if ref else 0), val, size - 1, COL_VAL, anchor)

    def note(self, x, y, lines, size=12.5, anchor="start"):
        self.mtext(x, y, lines, size, COL_NOTE, anchor, lh=size + 5)

    # ---------- 电源 / 地 ----------
    def gnd(self, x, y, kind="PGND", label=True):
        """接地符号，接入点在 (x, y)，符号向下生长。"""
        self.wire((x, y), (x, y + 12))
        if kind in ("AGND", "PE"):
            # 三角形（模拟地） / 大地符号
            if kind == "AGND":
                self.add(f'<path d="M{x-13},{y+12} L{x+13},{y+12} L{x},{y+30} Z" '
                         f'fill="none" stroke="{COL_SYM}" stroke-width="{LWS}"/>')
            else:
                for i, hw in enumerate((15, 10, 5)):
                    yy = y + 12 + i * 6
                    self.wire((x - hw, yy), (x + hw, yy), width=LWS)
                self.add(f'<circle cx="{x}" cy="{y+18}" r="20" fill="none" '
                         f'stroke="{COL_SYM}" stroke-width="1" stroke-dasharray="3 3"/>')
        else:
            for i, hw in enumerate((15, 9, 4)):
                yy = y + 12 + i * 6
                self.wire((x - hw, yy), (x + hw, yy), width=LWS)
        if label:
            self.text(x, y + 48, kind, 11.5, "#555", "middle")

    def vcc(self, x, y, name, up=True):
        """电源符号，接入点 (x, y)，向上生长。"""
        d = -1 if up else 1
        self.wire((x, y), (x, y + 14 * d))
        self.wire((x - 12, y + 14 * d), (x + 12, y + 14 * d), width=LWS)
        self.text(x, y + (14 * d) + (-8 if up else 20), name, 12.5,
                  COL_NET, "middle", "bold")

    def netlabel(self, x, y, name, side="right"):
        """网络标号（带箭头小旗）。"""
        w = 9.0 * len(name) + 18
        if side == "right":
            pts = f"{x},{y} {x+12},{y-11} {x+w},{y-11} {x+w},{y+11} {x+12},{y+11}"
            tx, ta = x + 20, "start"
        else:
            pts = f"{x},{y} {x-12},{y-11} {x-w},{y-11} {x-w},{y+11} {x-12},{y+11}"
            tx, ta = x - 20, "end"
        self.add(f'<polygon points="{pts}" fill="#f3e8f8" stroke="{COL_NET}" '
                 f'stroke-width="1.2"/>')
        self.text(tx, y + 5, name, 12.5, COL_NET, ta, "bold")

    # ---------- 两端元件 ----------
    def res_v(self, x, y, ref="", val="", side="right", lab_dy=22):
        self.wire((x, y), (x, y + 10))
        self.rect(x - 9, y + 10, 18, 40, fill="#fff")
        self.wire((x, y + 50), (x, y + 60))
        dx, a = (16, "start") if side == "right" else (-16, "end")
        self.rv(x + dx, y + lab_dy, ref, val, a)
        return {"1": (x, y), "2": (x, y + 60)}

    def res_h(self, x, y, ref="", val="", side="top"):
        self.wire((x, y), (x + 10, y))
        self.rect(x + 10, y - 9, 40, 18, fill="#fff")
        self.wire((x + 50, y), (x + 60, y))
        dy = -33 if side == "top" else 26
        self.rv(x + 30, y + dy, ref, val, "middle", gap=15)
        return {"1": (x, y), "2": (x + 60, y)}

    def cap_v(self, x, y, ref="", val="", side="right"):
        self.wire((x, y), (x, y + 25))
        self.wire((x - 15, y + 25), (x + 15, y + 25), width=2.4)
        self.wire((x - 15, y + 35), (x + 15, y + 35), width=2.4)
        self.wire((x, y + 35), (x, y + 60))
        dx, a = (21, "start") if side == "right" else (-21, "end")
        self.rv(x + dx, y + 25, ref, val, a)
        return {"1": (x, y), "2": (x, y + 60)}

    def ecap_v(self, x, y, ref="", val="", side="right"):
        """电解电容，pin1 为正极。"""
        self.wire((x, y), (x, y + 24))
        self.wire((x - 15, y + 24), (x + 15, y + 24), width=2.6)
        self.add(f'<path d="M{x-15},{y+42} Q{x},{y+30} {x+15},{y+42}" fill="none" '
                 f'stroke="{COL_SYM}" stroke-width="2.6"/>')
        self.wire((x, y + 36), (x, y + 60))
        self.text(x - 21, y + 20, "+", 15, COL_SYM, "middle", "bold")
        dx, a = (21, "start") if side == "right" else (-21, "end")
        self.rv(x + dx, y + 26, ref, val, a)
        return {"1": (x, y), "2": (x, y + 60)}

    def _tri_v(self, x, y, down=True, fill="#fff"):
        """二极管三角，down=True 表示导通方向向下。"""
        if down:
            return (f'<path d="M{x-13},{y} L{x+13},{y} L{x},{y+20} Z" fill="{fill}" '
                    f'stroke="{COL_SYM}" stroke-width="{LWS}" stroke-linejoin="round"/>')
        return (f'<path d="M{x-13},{y+20} L{x+13},{y+20} L{x},{y} Z" fill="{fill}" '
                f'stroke="{COL_SYM}" stroke-width="{LWS}" stroke-linejoin="round"/>')

    def diode_v(self, x, y, ref="", val="", down=True, kind="std", side="right"):
        """
        竖直二极管，长度 60。down=True → 阳极在 pin1(上)，阴极在 pin2(下)。
        kind: std / zener / schottky / tvs(双向) / led
        """
        self.wire((x, y), (x, y + 20))
        if kind == "tvs":
            # 双向 TVS：背靠背两个三角
            self.add(self._tri_v(x, y + 20, True))
            self.add(self._tri_v(x, y + 20, False))
            self.wire((x - 13, y + 30), (x + 13, y + 30), width=2.4)
            self.wire((x, y + 40), (x, y + 60))
        else:
            self.add(self._tri_v(x, y + 20, down))
            by = y + 40 if down else y + 20
            self.wire((x - 13, by), (x + 13, by), width=2.4)
            if kind == "zener":
                self.wire((x - 13, by), (x - 13, by + (7 if down else -7)), width=2.4)
                self.wire((x + 13, by), (x + 13, by - (7 if down else -7)), width=2.4)
            if kind == "schottky":
                s = 7 if down else -7
                self.wire((x - 13, by + s), (x - 13, by), (x - 6, by), width=2.4)
                self.wire((x + 13, by - s), (x + 13, by), (x + 6, by), width=2.4)
            self.wire((x, y + 40), (x, y + 60))
        if kind == "led":
            for k, off in ((0, 0), (1, 9)):
                ax, ay = x + 16 + off, y + 30 - off
                self.add(f'<path d="M{ax},{ay} l11,-11 M{ax+11},{ay-11} l-4,0.5 '
                         f'M{ax+11},{ay-11} l-0.5,4" stroke="{COL_SYM}" '
                         f'stroke-width="1.4" fill="none"/>')
        dx, a = (22, "start") if side == "right" else (-22, "end")
        if kind == "led":
            dx, a = (44, "start") if side == "right" else (-22, "end")
        self.rv(x + dx, y + 26, ref, val, a)
        return {"A" if down else "K": (x, y), "K" if down else "A": (x, y + 60),
                "1": (x, y), "2": (x, y + 60)}

    def fuse_h(self, x, y, ref="", val="", side="top"):
        self.wire((x, y), (x + 10, y))
        self.rect(x + 10, y - 10, 40, 20, fill="#fff")
        self.wire((x + 10, y), (x + 50, y), width=LWS)
        self.wire((x + 50, y), (x + 60, y))
        dy = -33 if side == "top" else 28
        self.rv(x + 30, y + dy, ref, val, "middle", gap=15)
        return {"1": (x, y), "2": (x + 60, y)}

    def fuse_v(self, x, y, ref="", val="", side="right"):
        self.wire((x, y), (x, y + 10))
        self.rect(x - 10, y + 10, 20, 40, fill="#fff")
        self.wire((x, y + 10), (x, y + 50), width=LWS)
        self.wire((x, y + 50), (x, y + 60))
        dx, a = (17, "start") if side == "right" else (-17, "end")
        self.rv(x + dx, y + 24, ref, val, a)
        return {"1": (x, y), "2": (x, y + 60)}

    # ---------- 有源器件 ----------
    def pmos_h(self, x, y, ref="", val=""):
        """
        P 沟道 MOSFET，横向电流：S 在左 (x, y)，D 在右 (x+80, y)，G 引脚向下。
        体二极管方向 D→S（正常工作时反偏，可真正阻断上电冲击）。
        """
        gx = x + 40                      # 栅极竖线 x
        cx = x + 56                      # 沟道竖线 x
        self.wire((x, y), (cx, y))                       # S 引线
        self.wire((x + 80, y), (cx, y))                  # D 引线（同一水平线）
        # 沟道三段
        self.wire((cx, y - 26), (cx, y + 26), width=2.6)
        # S / D 折线接到沟道
        self.wire((x, y), (x, y - 20), (cx, y - 20))     # S → 沟道上段
        self.wire((x + 80, y), (x + 80, y + 20), (cx, y + 20))  # D → 沟道下段
        # 栅极板
        self.wire((gx, y - 26), (gx, y + 26), width=2.4)
        self.wire((gx, y), (gx, y + 60))                 # G 引出向下
        # P 沟道箭头：由沟道指向栅极
        self.add(f'<path d="M{cx-2},{y} l-12,0 M{gx+10},{y} l-6,-4.5 l0,9 Z" '
                 f'fill="{COL_SYM}" stroke="{COL_SYM}" stroke-width="1.4"/>')
        # 体二极管 D→S（画在右侧，阳极接 D 段、阴极接 S 段）
        bx = cx + 26
        self.wire((cx, y - 20), (bx, y - 20), width=1.2)
        self.wire((cx, y + 20), (bx, y + 20), width=1.2)
        self.wire((bx, y + 20), (bx, y + 8), width=1.2)
        self.add(f'<path d="M{bx-7},{y+8} L{bx+7},{y+8} L{bx},{y-6} Z" fill="none" '
                 f'stroke="{COL_SYM}" stroke-width="1.2"/>')
        self.wire((bx - 7, y - 6), (bx + 7, y - 6), width=1.6)
        self.wire((bx, y - 6), (bx, y - 20), width=1.2)
        self.text(x + 4, y - 30, "S", 11, "#666")
        self.text(x + 70, y + 34, "D", 11, "#666")
        self.text(gx - 14, y + 40, "G", 11, "#666")
        self.rv(x + 6, y - 62, ref, val, "start")
        return {"S": (x, y), "D": (x + 80, y), "G": (gx, y + 60)}

    def npn_v(self, x, y, ref="", val=""):
        """NPN，C 在上 (x, y)，E 在下 (x, y+80)，B 在左 (x-60, y+40)。"""
        cx = x - 14
        self.wire((x, y), (x, y + 22), (cx, y + 30))
        self.wire((x, y + 80), (x, y + 58), (cx, y + 50))
        self.wire((cx, y + 16), (cx, y + 64), width=2.6)
        self.wire((x - 60, y + 40), (cx - 12, y + 40), width=LWS)
        self.wire((cx - 12, y + 16), (cx - 12, y + 64), width=2.4)
        self.add(f'<path d="M{x-4},{y+56} l-6.5,-1 l3,6 Z" fill="{COL_SYM}"/>')
        self.add(f'<circle cx="{x-8}" cy="{y+40}" r="30" fill="none" '
                 f'stroke="{COL_SYM}" stroke-width="1.2"/>')
        self.rv(x + 28, y + 16, ref, val, "start")
        return {"C": (x, y), "E": (x, y + 80), "B": (x - 60, y + 40)}

    def nmos_v(self, x, y, ref="", val=""):
        """N 沟道 MOSFET，D 在上 (x, y)，S 在下 (x, y+80)，G 在左 (x-60, y+40)。"""
        cx = x - 14
        self.wire((x, y), (x, y + 20), (cx, y + 20))
        self.wire((x, y + 80), (x, y + 60), (cx, y + 60))
        self.wire((cx, y + 14), (cx, y + 66), width=2.6)
        self.wire((x - 60, y + 40), (cx - 14, y + 40), width=LWS)
        self.wire((cx - 14, y + 14), (cx - 14, y + 66), width=2.4)
        self.add(f'<path d="M{cx-2},{y+40} l-10,0 M{cx-2},{y+40} l-7,-4.5 l0,9 Z" '
                 f'fill="{COL_SYM}" stroke="{COL_SYM}" stroke-width="1.3"/>')
        self.wire((cx, y + 40), (x, y + 40), width=1.2)
        self.rv(x + 22, y + 16, ref, val, "start")
        return {"D": (x, y), "S": (x, y + 80), "G": (x - 60, y + 40)}

    def opto(self, x, y, ref="", val="PC817C"):
        """
        4 脚光耦，本体 100x110，左上 pin1(A)、左下 pin2(K)、右下 pin3(E)、右上 pin4(C)。
        引脚伸出 30px。
        """
        w, h = 100, 110
        self.rect(x, y, w, h, fill="#fff", rx=3)
        p1, p2 = (x - 30, y + 28), (x - 30, y + 82)
        p4, p3 = (x + w + 30, y + 28), (x + w + 30, y + 82)
        for p, xe in ((p1, x), (p2, x)):
            self.wire(p, (xe, p[1]))
        for p, xe in ((p4, x + w), (p3, x + w)):
            self.wire(p, (xe, p[1]))
        # LED（左半）
        lx = x + 26
        self.wire((x, y + 28), (lx, y + 28), (lx, y + 44), width=LWS)
        self.add(f'<path d="M{lx-11},{y+44} L{lx+11},{y+44} L{lx},{y+64} Z" '
                 f'fill="#fff" stroke="{COL_SYM}" stroke-width="{LWS}"/>')
        self.wire((lx - 11, y + 64), (lx + 11, y + 64), width=2.4)
        self.wire((lx, y + 64), (lx, y + 82), (x, y + 82), width=LWS)
        for k in range(2):
            ax, ay = lx + 16 + k * 9, y + 58 - k * 9
            self.add(f'<path d="M{ax},{ay} l10,-10 M{ax+10},{ay-10} l-4,0.7 '
                     f'M{ax+10},{ay-10} l-0.7,4" stroke="{COL_SYM}" '
                     f'stroke-width="1.3" fill="none"/>')
        # 光敏三极管（右半）
        tx = x + w - 22
        self.wire((x + w, y + 28), (tx, y + 28), (tx - 12, y + 40), width=LWS)
        self.wire((x + w, y + 82), (tx, y + 82), (tx - 12, y + 70), width=LWS)
        self.wire((tx - 12, y + 34), (tx - 12, y + 76), width=2.6)
        self.add(f'<path d="M{tx-2},{y+76} l-6.5,-1.5 l3,6 Z" fill="{COL_SYM}"/>')
        self.text(x - 34, y + 24, "1", 11, "#666", "end")
        self.text(x - 34, y + 78, "2", 11, "#666", "end")
        self.text(x + w + 34, y + 24, "4", 11, "#666")
        self.text(x + w + 34, y + 78, "3", 11, "#666")
        self.text(x + w / 2, y - 22, ref, 14, COL_REF, "middle", "bold")
        self.text(x + w / 2, y + h + 22, val, 12.5, COL_VAL, "middle")
        return {"A": p1, "K": p2, "C": p4, "E": p3,
                "1": p1, "2": p2, "4": p4, "3": p3}

    # ---------- 连接器 / 模块 ----------
    def conn(self, x, y, pins, ref="", val="", side="right", pitch=32, w=54):
        """
        端子/连接器。pins 为引脚名列表（从上到下）。
        side="right"：引脚从本体右侧引出；"left" 则从左侧引出。
        返回 {引脚序号(1起): (x, y)} 以及 {引脚名: (x, y)}。
        """
        n = len(pins)
        h = pitch * n + 16
        bx = x if side == "right" else x - w
        self.rect(bx, y, w, h, fill=COL_BOX, rx=3)
        out = {}
        for i, nm in enumerate(pins):
            py = y + 16 + i * pitch
            self.rect(bx + (w - 16 if side == "right" else 0), py - 7, 16, 14,
                      fill="#fff")
            ex = bx + w + 62 if side == "right" else bx - 62
            self.wire((bx + (w if side == "right" else 0), py), (ex, py))
            self.text(bx + w / 2 - (10 if side == "right" else -10), py + 5,
                      str(i + 1), 11.5, "#666", "middle")
            tx = bx + w + 6 if side == "right" else bx - 6
            self.text(tx, py - 7, nm, 11.5, "#333",
                      "start" if side == "right" else "end")
            out[str(i + 1)] = (ex, py)
            out[nm] = (ex, py)
        self.text(bx + w / 2, y - 26, ref, 14, COL_REF, "middle", "bold")
        if val:
            self.text(bx + w / 2, y - 10, val, 11.5, "#555", "middle")
        out["_box"] = (bx, y, w, h)
        return out

    def block(self, x, y, w, h, title, sub=None, fill=None, dash=None):
        self.rect(x, y, w, h, fill=fill or COL_BOX, rx=5, sw=2.0, dash=dash)
        self.text(x + w / 2, y + (26 if sub else h / 2 + 6), title, 15,
                  "#111", "middle", "bold")
        if sub:
            for i, s in enumerate(sub):
                self.text(x + w / 2, y + 48 + i * 18, s, 12, "#444", "middle")
        return {"L": (x, y + h / 2), "R": (x + w, y + h / 2),
                "T": (x + w / 2, y), "B": (x + w / 2, y + h)}

    def region(self, x, y, w, h, label, color=COL_DASH):
        self.rect(x, y, w, h, fill="none", stroke=color, sw=2.2, rx=8, dash="12 7")
        self.add(f'<rect x="{x+14}" y="{y-13}" width="{9.5*len(label)+20}" '
                 f'height="26" rx="4" fill="#fff" stroke="{color}" stroke-width="1.2"/>')
        self.text(x + 24, y + 5, label, 13.5, color, "start", "bold")

    # ---------- 图框 ----------
    def frame(self):
        m = 18
        self.rect(m, m, self.w - 2 * m, self.h - 2 * m, fill="none",
                  stroke="#333", sw=2.0)
        bw, bh = 460, 96
        bx, by = self.w - m - bw, self.h - m - bh
        self.rect(bx, by, bw, bh, fill="#fff", stroke="#333", sw=2.0)
        self.wire((bx, by + 36), (bx + bw, by + 36), width=1.2)
        self.wire((bx + 320, by), (bx + 320, by + bh), width=1.2)
        self.text(bx + 12, by + 24, self.project, 13, "#111", "start", "bold")
        self.text(bx + 12, by + 60, self.no, 15, "#111", "start", "bold")
        self.text(bx + 12, by + 82, self.short, 12, "#444", "start")
        self.text(bx + 332, by + 24, "图  号", 11.5, "#666")
        self.text(bx + 448, by + 24, self.no, 12.5, "#111", "end")
        self.text(bx + 332, by + 58, f"版本  {self.rev}", 11.5, "#444")
        self.text(bx + 332, by + 82, self.date, 11.5, "#444")
        self.text(m + 14, m + 34, self.title, 20, "#111", "start", "bold")

    def svg(self):
        return ('<svg xmlns="http://www.w3.org/2000/svg" '
                f'width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}">'
                f'<rect width="{self.w}" height="{self.h}" fill="#ffffff"/>'
                + "".join(self.el) + "</svg>")

    def save(self, path):
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.svg())
        return path
