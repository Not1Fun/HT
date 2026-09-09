# -*- coding: utf-8 -*-
"""SH1 — 系统电源分配总图（对应需求 Visio 拓扑的电源段）。

本图为系统级框图，标明哪些器件在机箱/导轨上、哪些落到 PWR-01 电源板。
按方案 A：220V 强电段全部走机箱线，DRS-240-36 为导轨件，均不上 PCB。
"""
from schlib import Sheet, COL_OFFBRD, COL_SYM, COL_NOTE, COL_REF

W, H = 2100, 1240
OFF = COL_OFFBRD

# ---- 主要 y 坐标 ----
yL, yN, yPE = 200, 260, 320          # AC 三线
ySIG = (285, 325, 365, 405)          # DRS Form C 四组信号
yBP, yBN = 560, 615                  # 电池 / 母线 正负
yBAT_P, yBAT_N = 810, 865            # 电池支路
# ---- 主要 x 坐标 ----
DX, DY, DW, DH = 830, 150, 300, 560  # DRS 模块
RX, RY, RW, RH = 1180, 200, 760, 660  # PWR-01 板界


def build():
    s = Sheet(W, H, "SH1",
              "SH1  系统电源分配总图 — AC 进线 / 电池 / DRS-240-36 / 36V 母线",
              short="系统电源分配总图")
    s.frame()

    # ==================== 图例 ====================
    lx, ly = 60, 1010
    s.rect(lx, ly, 440, 142, fill="#fff", stroke="#999", sw=1.4, rx=5)
    s.text(lx + 14, ly + 28, "图例", 14, "#111", "start", "bold")
    s.rect(lx + 18, ly + 44, 36, 20, fill=OFF, sw=1.6)
    s.text(lx + 66, ly + 59, "板外件：机箱安装 / DIN 导轨 / 面板", 12.5, "#333")
    s.rect(lx + 18, ly + 76, 36, 20, fill="#f7f9fc", sw=1.6)
    s.text(lx + 66, ly + 91, "PWR-01 电源板上的功能块", 12.5, "#333")
    s.wire((lx + 18, ly + 118), (lx + 54, ly + 118), width=4.0)
    s.text(lx + 66, ly + 123, "功率回路（≥3A，加粗）", 12.5, "#333")

    # ==================== 一、AC 进线（全部板外） ====================
    x1 = s.conn(80, 184, ["L", "N", "PE"], "X1", "IEC 三孔插座", pitch=60)
    s.text(107, 162, "三孔电源插座", 13, "#333", "middle")

    s.block(250, 165, 130, 120, "AC 开关", ["DPST 16A", "250VAC 船型"], fill=OFF)
    s.wire(x1["L"], (250, yL))
    s.wire(x1["N"], (250, yN))
    s.wire(x1["PE"], (196, 470), (600, 470), (600, 360))
    s.gnd(400, 470, "PE")
    s.text(430, 462, "PE → 机箱外壳 / DRS FG / 面板 / 变压器磁芯", 12, "#333")

    s.wire((380, yL), (400, yL))
    f = s.fuse_h(400, yL, "F_AC", "T3.15A 250V  ≥1500A 分断")
    s.wire(f["2"], (600, yL))
    s.wire((380, yN), (600, yN))

    # 压敏电阻 RV1（跨接 L-N）
    mx = 520
    s.wire((mx, yL), (mx, yL + 12))
    s.rect(mx - 11, yL + 12, 22, 36, fill="#fff")
    s.add(f'<path d="M{mx-15},{yL+47} L{mx+15},{yL+13}" stroke="{COL_SYM}" '
          f'stroke-width="1.7"/>')
    s.add(f'<path d="M{mx+15},{yL+13} l-2,7.5 M{mx+15},{yL+13} l-7.5,2" '
          f'stroke="{COL_SYM}" stroke-width="1.7" fill="none"/>')
    s.wire((mx, yL + 48), (mx, yN))
    s.dot(mx, yL)
    s.dot(mx, yN)
    s.rv(mx, 292, "RV1", "TMOV14 320Vac", "middle")
    s.text(mx, 328, "（必须带热脱扣）", 11.5, COL_NOTE, "middle")

    s.block(600, 160, 150, 200, "EMI 滤波", ["共模 + 差模", "板外整体模块"], fill=OFF)
    s.wire((750, yL), (DX, yL))
    s.wire((750, yN), (DX, yN))
    s.note(400, 128, ["※ 方案 A：220V 段全部走机箱线与航空插头，不上 PCB"])

    # ==================== 二、DRS-240-36（导轨件） ====================
    s.rect(DX, DY, DW, DH, fill=OFF, stroke="#333", sw=2.4, rx=6)
    s.text(DX + DW / 2, DY + 32, "DRS-240-36", 18, "#111", "middle", "bold")
    s.text(DX + DW / 2, DY + 54, "MEAN WELL  240W / 36V / 6.6A", 12, "#444", "middle")
    s.text(DX + DW / 2, DY + 72, "AC-DC + 充电器 + DC-UPS", 12, "#444", "middle")
    s.text(DX + DW / 2, DY + 92, "DIN 导轨件 · 不上 PCB", 12, COL_NOTE, "middle")

    for yy, nm in ((yL, "L"), (yN, "N"), (yBP, "bat+"), (yBN, "bat−")):
        s.rect(DX - 1, yy - 9, 18, 18, fill="#fff")
        s.text(DX + 26, yy + 5, nm, 12.5, "#333")
    for yy, nm in ((yBP, "输出+"), (yBN, "输出−")):
        s.rect(DX + DW - 17, yy - 9, 18, 18, fill="#fff")
        s.text(DX + DW - 26, yy + 5, nm, 12.5, "#333", "end")

    sig = [("AC_OK", "2 · 3"), ("DC_OK", "5 · 6"),
           ("BAT_OK", "8 · 9"), ("CHG_OK", "11 · 12")]
    s.rect(DX + 20, 258, DW - 40, 174, fill="#fff", stroke="#666", sw=1.3, rx=3)
    s.text(DX + DW / 2, 278, "Form C 信号触点（干接点）", 11.5, "#333", "middle", "bold")
    for (nm, pins), yy in zip(sig, ySIG):
        s.text(DX + 34, yy + 5, nm, 12, "#333")
        s.text(DX + DW - 34, yy + 5, pins, 12, "#666", "end")
        s.rect(DX + DW - 17, yy - 9, 18, 18, fill="#fff")
    s.note(DX + 4, DY + DH + 34, [
        "※ DIP 必须拨到 LiFePO₄ 充电曲线（出厂默认铅酸 3-stage）",
        "   Vboost 43.2V = 3.60 V/cell    Vfloat 41.4V = 3.45 V/cell",
        "※ 只用「正常」触点 2-3 / 5-6 / 8-9 / 11-12；接线前按 datasheet 核对 NO/NC",
        "※ 4 组 COM 是否内部共通未知，故 J2 用 8P（每组独立两根线）",
    ])

    # ==================== 三、电池支路（全部板外） ====================
    s.block(90, 760, 200, 140, "电池 + BMS",
            ["12S LiFePO₄", "38.4 V / 10 Ah", "须有低温禁充保护"], fill=OFF)
    s.wire((290, yBAT_P), (330, yBAT_P))
    fb = s.fuse_h(330, yBAT_P, "F_BAT", "T15A · ≥58VDC 直流额定")
    s.wire(fb["2"], (450, yBAT_P))
    s.wire((290, yBAT_N), (450, yBAT_N))
    s.block(450, 755, 170, 150, "DC 断路器", ["隔离开关 2P", "≥63VDC / 16A"], fill=OFF)
    s.wire((620, yBAT_P), (700, yBAT_P), (700, yBP), (DX, yBP), width=4.0)
    s.wire((620, yBAT_N), (760, yBAT_N), (760, yBN), (DX, yBN), width=4.0)
    s.note(92, 940, [
        "※ 熔断器紧靠电池正极安装，必须选有直流额定的型号",
        "   （12S LiFePO₄ 短路电流可达数百安培，交流额定不能代用）",
    ])

    # ==================== 四、PWR-01 板界 ====================
    s.region(RX, RY, RW, RH, "PWR-01  电源板（本次设计范围）")

    # 36V 母线进板
    s.wire((DX + DW, yBP), (1245, yBP), width=4.0)
    s.wire((DX + DW, yBN), (1245, yBN), width=4.0)
    s.text(1155, 522, "36V 母线", 14, "#111", "middle", "bold")
    s.text(1155, 660, "DRS 输出 30 – 43.2V", 11.5, COL_NOTE, "middle")

    j1 = s.conn(1245, 545, ["+36V_RAW", "PGND"], "J1", "2P 5.08mm 端子", pitch=42)
    s.wire((1245, yBP), (1245, 561), width=4.0)
    s.wire((1245, yBN), (1245, 603), width=4.0)

    s.block(1470, 505, 280, 190, "36V 母线处理",
            ["F1 熔断 + Q1 软启动", "TVS / 反接 crowbar / 体电容",
             "母线分压 → V_BUS_SENSE", "▸ 详见 SH2"])
    s.wire(j1["1"], (1470, 561), width=4.0)
    s.wire(j1["2"], (1420, 603), (1420, 640), (1470, 640), width=4.0)

    # DRS 状态隔离
    j2 = s.conn(1245, 269, ["AC_OK ×2", "DC_OK ×2", "BAT_OK ×2", "CHG_OK ×2"],
                "J2", "8P 3.5mm 端子", pitch=40)
    for i, yy in enumerate(ySIG):
        s.wire((DX + DW, yy), (1245, yy))
        s.wire(j2[str(i + 1)], (1470, yy))

    s.block(1470, 250, 280, 200, "DRS 状态隔离",
            ["4 × PC817C", "1N4148 反并联保护", "低有效：触点闭合 = 正常",
             "▸ 详见 SH3"])

    j3 = s.conn(1800, 250, ["3V3_D", "DGND", "STAT_AC_OK", "STAT_DC_OK",
                            "STAT_BAT_OK", "STAT_CHG_OK", "V_BUS_SENSE"],
                "J3", "IDC 10P → 控制板", pitch=28, w=44)
    s.wire((1750, 320), (1800, 320))
    s.wire((1750, 600), (1775, 600), (1775, 418), (1800, 418))

    # 12V_AUX 回灌给光耦一次侧（用网络标号，避免长距离绕线）
    s.wire((1520, 450), (1520, 490), (1400, 490))
    s.netlabel(1400, 490, "+12V_AUX ← J7", "left")

    # ==================== 五、板外负载 ====================
    loads = [
        (1210, 900, 190, 130, "DDR-30L-12", ["36V → 12V / 2.5A", "18–75V 输入 · 导轨件"],
         1500, 760, "J4  2P · F2 T2A"),
        (1440, 900, 190, 130, "DDR-30L-5", ["36V → 5V", "18–75V 输入 · 导轨件"],
         1590, 800, "J5  2P · F3 T2A"),
        (1670, 900, 210, 130, "TPA3255 功放", ["PVDD 30 – 43V", "≈2A 连续"],
         1690, 845, "J6  大电流 · F1 T10A"),
    ]
    for bx, by, bw, bh, t, sub, vx, hy, lab in loads:
        s.block(bx, by, bw, bh, t, sub, fill=OFF)
        s.wire((vx, 695), (vx, hy), (bx + bw / 2, hy), (bx + bw / 2, by), width=4.0)
        s.text(vx + 10, (695 + hy) / 2 + 4, lab, 11.5, COL_REF)

    s.wire((1210, 965), (1160, 965))
    s.netlabel(1160, 965, "+12V_AUX → J7", "left")

    s.note(700, 1040, [
        "※ 三个 DC/DC 与功放均为板外件；PWR-01 只提供熔断、软启动、体电容与端子",
        "※ DDR-30L 后缀 L = 18~75VDC（G 才是 9~36VDC）——本母线 30~43.2V 摆幅",
        "   正确落在 L 档内，此处选型无误，采购时勿按字面误换成 G 版",
        "※ 全机单点星型接地：PGND / GND_AUX / DGND / AGND 在 PWR-01 上汇合后接 PE",
    ])
    return s


if __name__ == "__main__":
    build().save("out/SH1-系统电源分配总图.svg")
