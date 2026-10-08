#!/usr/bin/env python3
"""Generate ring-flex-v1.kicad_sch from the netlist in ../NETLIST.md.

Every pin gets a short stub wire and a net label; nothing is routed by hand.
The result is electrically complete and ERC-checkable, which is the point --
pretty placement is for the human in the schematic editor afterwards.
"""
import os, re, uuid, sys

SYMDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"
LOCAL  = "ring-flex-v1.kicad_sym"

def sexp_block(text, start):
    """Return the balanced-paren block beginning at index start."""
    depth, i, instr = 0, start, False
    while i < len(text):
        c = text[i]
        if instr:
            if c == '\\': i += 2; continue
            if c == '"': instr = False
        elif c == '"': instr = True
        elif c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: return text[start:i+1]
        i += 1
    raise ValueError("unbalanced")

def load_symbol(lib, name):
    path = LOCAL if lib == "ring-flex-v1" else os.path.join(SYMDIR, lib + ".kicad_sym")
    text = open(path).read()
    m = re.search(r'\(symbol "%s"' % re.escape(name), text)
    if not m: raise SystemExit("symbol not found: %s:%s" % (lib, name))
    blk = sexp_block(text, m.start())
    ext = re.search(r'\(extends "([^"]+)"\)', blk)
    if ext:
        parent = re.search(r'\(symbol "%s"' % re.escape(ext.group(1)), text)
        pblk = sexp_block(text, parent.start())
        # keep the child's identity, take the parent's pins and graphics
        pblk = pblk.replace('(symbol "%s' % ext.group(1), '(symbol "%s' % name, 1)
        pblk = re.sub(r'\(symbol "%s_' % re.escape(ext.group(1)), '(symbol "%s_' % name, pblk)
        blk = pblk
    # Embedded symbols are named with the full lib_id, exactly as KiCad writes
    # them. The library form also carries (in_pos_files ...), which the
    # schematic parser rejects: in_pos_files, do_not_autoplace, show_name.
    # That is what made the file fail to load, not the name.
    # Only the OUTER symbol takes the library prefix. The unit sub-symbols
    # keep their plain names -- prefixing those is what KiCad rejects.
    blk = blk.replace('(symbol "%s"' % name, '(symbol "%s:%s"' % (lib, name), 1)
    for tok in ('in_pos_files', 'do_not_autoplace', 'show_name'):
        blk = re.sub(r'\n\s*\(%s[^)]*\)' % tok, '', blk)
    return blk

def pins_of(blk):
    """[(x, y, angle, name, number)] in symbol coordinates."""
    out = []
    for m in re.finditer(r'\(pin\s+\S+\s+\S+\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)', blk):
        seg = sexp_block(blk, m.start())
        nm = re.search(r'\(name "([^"]*)"', seg)
        nu = re.search(r'\(number "([^"]*)"', seg)
        out.append((float(m.group(1)), float(m.group(2)), float(m.group(3)),
                    nm.group(1) if nm else "", nu.group(1) if nu else ""))
    return out

U = lambda: str(uuid.uuid4())

# ---- the design: ref -> (lib, symbol, value, x, y, {pin number or name: net})
STUB = 3.81
P = []   # parts
GRID = 1.27
def snap(v):
    return round(v / GRID) * GRID

def part(ref, lib, sym, value, x, y, nets, fp=""):
    # Everything must land on the 1.27 mm schematic grid or KiCad does not
    # treat a wire endpoint as touching a pin.
    P.append(dict(ref=ref, lib=lib, sym=sym, value=value,
                  x=snap(x), y=snap(y), nets=nets, fp=fp))

# --- MCU
part("U1", "ring-flex-v1", "MDBT50Q-1MV2", "MDBT50Q-1MV2", 60, 60, {
    "P1": "VBAT", "P2": "GND", "P3": "MOTOR_PWM", "P4": "MOTOR_EN",
    "P5": "CHG_STAT", "P6": "SWDIO", "P7": "SWCLK"})
# --- motor stage
part("Q2", "Device", "Q_NMOS", "AO3400A", 160, 60, {"G": "MOTOR_GATE", "D": "MOTOR_SW", "S": "GND"}, fp="Package_TO_SOT_SMD:SOT-23")
part("M1", "Motor", "Motor_DC", "NFP-C1020 10x2.0mm ERM", 160, 30, {"1": "V_MOTOR", "2": "MOTOR_SW"},
     fp="ring-flex-v1:Motor_Coin_10mm_WirePads")
# D1 flyback: cathode (pin 1 = K) to the supply, anode to the switched node
part("D1", "Device", "D_Schottky", "BAT54W", 200, 45, {"1": "V_MOTOR", "2": "MOTOR_SW"}, fp="Diode_SMD:D_SOD-323")
part("R1", "Device", "R", "10k", 130, 90, {"1": "MOTOR_GATE", "2": "GND"}, fp="Resistor_SMD:R_0402_1005Metric")
part("R2", "Device", "R", "1k", 110, 60, {"1": "MOTOR_PWM", "2": "MOTOR_GATE"}, fp="Resistor_SMD:R_0402_1005Metric")
part("C1", "Device", "C", "100uF", 235, 45, {"1": "V_MOTOR", "2": "GND"}, fp="Capacitor_SMD:C_1206_3216Metric")
# --- motor rail LDO, enable gated by the MCU
part("U3", "Regulator_Linear", "AP2112K-3.3", "TPS7A0230PDBVR 3.0V", 110, 135, {
    "1": "VBAT", "2": "GND", "3": "MOTOR_EN", "5": "V_MOTOR"},
     fp="Package_TO_SOT_SMD:SOT-23-5")
part("C6", "Device", "C", "1uF", 155, 145, {"1": "V_MOTOR", "2": "GND"}, fp="Capacitor_SMD:C_0402_1005Metric")
# --- charger
part("U2", "Battery_Management", "MCP73831-2-OT", "MCP73831-2", 55, 135, {
    "1": "CHG_STAT", "2": "GND", "3": "VBAT", "4": "VCHG_IN", "5": "PROG"},
     fp="Package_TO_SOT_SMD:SOT-23-5")
part("R3", "Device", "R", "66.5k", 30, 165, {"1": "PROG", "2": "GND"}, fp="Resistor_SMD:R_0402_1005Metric")
part("C4", "Device", "C", "1uF", 15, 135, {"1": "VCHG_IN", "2": "GND"}, fp="Capacitor_SMD:C_0402_1005Metric")
part("C3", "Device", "C", "4.7uF", 85, 165, {"1": "VBAT", "2": "GND"}, fp="Capacitor_SMD:C_0603_1608Metric")
# D2 reverse-polarity guard: anode on the contact, cathode feeding the charger
part("D2", "Device", "D_Schottky", "BAT54W", 15, 110, {"1": "VCHG_IN", "2": "VCHG"}, fp="Diode_SMD:D_SOD-323")
# --- cell and protection (DW01A + FS8205A back-to-back FETs, common source)
part("BT1", "Device", "Battery_Cell", "LiPo ~30mAh 16x8x2.5", 55, 205, {"1": "VBAT", "2": "CELL_N"},
     fp="ring-flex-v1:Battery_LiPo_16x8_WirePads")
part("U4", "Battery_Management", "DW01A", "DW01A", 125, 205, {
    "5": "DW_VCC", "6": "CELL_N", "1": "DW_OD", "3": "DW_OC", "2": "DW_CS", "4": None},
     fp="Package_TO_SOT_SMD:SOT-23-6")
part("R4", "Device", "R", "1k", 95, 180, {"1": "VBAT", "2": "DW_VCC"}, fp="Resistor_SMD:R_0402_1005Metric")
part("R5", "Device", "R", "2k", 150, 230, {"1": "DW_CS", "2": "GND"},
     fp="Resistor_SMD:R_0402_1005Metric")
part("C5", "Device", "C", "0.1uF", 95, 230, {"1": "DW_VCC", "2": "GND"}, fp="Capacitor_SMD:C_0402_1005Metric")
part("Q1A", "Device", "Q_NMOS", "FS8205A Q1 (discharge)", 180, 200, {"G": "DW_OD", "S": "CELL_N", "D": "Q1_MID"}, fp="Package_TO_SOT_SMD:SOT-23-6")
part("Q1B", "Device", "Q_NMOS", "FS8205A Q2 (charge)", 220, 200, {"G": "DW_OC", "S": "GND", "D": "Q1_MID"}, fp="Package_TO_SOT_SMD:SOT-23-6")
part("C2", "Device", "C", "10uF", 255, 205, {"1": "VBAT", "2": "GND"}, fp="Capacitor_SMD:C_0805_2012Metric")
# --- contacts and test points
part("J1", "Connector", "TestPoint", "charge pad +", 15, 90, {"1": "VCHG"},
     fp="ring-flex-v1:ChargePad_2x2mm")
part("J2", "Connector", "TestPoint", "charge pad -", 35, 90, {"1": "GND"},
     fp="ring-flex-v1:ChargePad_2x2mm")
# USB-C, laid out but NOT populated. It is insurance for bring-up: if BLE DFU
# fights back, populating this gives the UF2/serial path the XIAO has today.
# CC1 and CC2 need their 5.1k pulldowns or a USB-C source supplies nothing.
part("J3", "Connector", "USB_C_Receptacle_PowerOnly_24P", "USB-C (DNP)", 35, 40, {
    "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
    "A4": "VCHG", "A9": "VCHG", "B4": "VCHG", "B9": "VCHG",
    "A5": "USB_CC1", "B5": "USB_CC2"},
     fp="Connector_USB:USB_C_Receptacle_HCTL_HC-TYPE-C-16P-01A")
part("R6", "Device", "R", "5.1k", 75, 35, {"1": "USB_CC1", "2": "GND"},
     fp="Resistor_SMD:R_0402_1005Metric")
part("R7", "Device", "R", "5.1k", 90, 35, {"1": "USB_CC2", "2": "GND"},
     fp="Resistor_SMD:R_0402_1005Metric")
for i, (ref, net) in enumerate([("TP1","SWDIO"),("TP2","SWCLK"),("TP3","VBAT"),("TP4","GND")]):
    part(ref, "Connector", "TestPoint", net, 235 + i*15, 105, {"1": net},
         fp="TestPoint:TestPoint_Pad_D1.0mm")
# --- power flags so ERC sees the rails as driven
# VBAT and V_MOTOR are already driven (charger VBAT pin, LDO VOUT), so a flag
# there would be two power outputs on one net. DW_VCC arrives through R4 and
# CELL_N is the cell's own negative, neither of which has a driving pin.
for i, (net, x) in enumerate([("GND", 270), ("VCHG_IN", 285), ("DW_VCC", 300), ("CELL_N", 315), ("VCHG", 330)]):
    part("#FLG%02d" % (i+1), "power", "PWR_FLAG", "PWR_FLAG", x, 155, {"1": net})

# ---- emit
libs, body = {}, []
for p in P:
    key = "%s:%s" % (p["lib"], p["sym"])
    if key not in libs:
        libs[key] = load_symbol(p["lib"], p["sym"])
    blk = libs[key]
    pins = pins_of(blk)
    # map by number, then by name
    by_num = {n: (x, y, a) for (x, y, a, nm, n) in pins}
    by_nam = {nm: (x, y, a) for (x, y, a, nm, n) in pins}
    sym_uuid = U()
    props = [
        ('Reference', p["ref"], 0, -14),
        ('Value', p["value"], 0, 14),
        ('Footprint', p["fp"], 0, 18),
    ]
    prop_s = "".join(
        '\n\t\t(property "%s" "%s" (at %.2f %.2f 0) (effects (font (size 1.27 1.27))%s))'
        % (k, v, p["x"], p["y"] + dy, " (hide yes)" if k == "Footprint" else "")
        for (k, v, dx, dy) in props)
    pin_s = "".join('\n\t\t(pin "%s" (uuid "%s"))' % (n, U()) for (_, _, _, _, n) in pins)
    body.append(
        '\n\t(symbol\n\t\t(lib_id "%s")\n\t\t(at %.2f %.2f 0)\n\t\t(unit 1)'
        '\n\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)'
        '\n\t\t(uuid "%s")%s%s'
        '\n\t\t(instances (project "ring-flex-v1" (path "/%s" (reference "%s") (unit 1))))\n\t)'
        % (key, p["x"], p["y"], sym_uuid, prop_s, pin_s, SHEET_UUID := globals().get("SHEET_UUID") or U(), p["ref"]))
    globals()["SHEET_UUID"] = SHEET_UUID
    # stubs + labels
    import math
    for pin_key, net in p["nets"].items():
        pos = by_num.get(pin_key) or by_nam.get(pin_key)
        if pos is None:
            sys.exit("ERROR %s: no pin %r on %s (have numbers %s names %s)"
                     % (p["ref"], pin_key, key, sorted(by_num), sorted(by_nam)))
        px, py, ang = pos
        # symbol space is Y-up, schematic is Y-down
        sx, sy = p["x"] + px, p["y"] - py
        if net is None:
            body.append('\n\t(no_connect (at %.2f %.2f) (uuid "%s"))' % (sx, sy, U()))
            continue
        # the pin's `at` is the connection end; its angle points into the body,
        # so the stub runs the opposite way
        import math
        dx, dy = -math.cos(math.radians(ang)), -math.sin(math.radians(ang))
        ex, ey = sx + STUB * dx, sy - STUB * dy
        body.append('\n\t(wire (pts (xy %.2f %.2f) (xy %.2f %.2f))'
                    '\n\t\t(stroke (width 0) (type solid)) (uuid "%s")\n\t)'
                    % (sx, sy, ex, ey, U()))
        lab_ang = 0 if dx >= 0 else 180
        body.append('\n\t(label "%s" (at %.2f %.2f %d)'
                    '\n\t\t(effects (font (size 1.27 1.27)) (justify left bottom)) (uuid "%s")\n\t)'
                    % (net, ex, ey, lab_ang, U()))

lib_s = "".join("\n\t\t" + libs[k].replace("\n", "\n\t\t") for k in libs)
out = ('(kicad_sch\n\t(version 20250610)\n\t(generator "glucose-ring")'
       '\n\t(generator_version "9.0")\n\t(uuid "%s")\n\t(paper "A3")'
       '\n\t(lib_symbols%s\n\t)%s'
       '\n\t(sheet_instances\n\t\t(path "/" (page "1"))\n\t)\n)\n'
       % (globals()["SHEET_UUID"], lib_s, "".join(body)))
open("ring-flex-v1.kicad_sch", "w").write(out)
print("wrote ring-flex-v1.kicad_sch: %d parts, %d symbols" % (len(P), len(libs)))
