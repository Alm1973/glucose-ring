#!/usr/bin/env python3
"""Generate ring-flex-v1.kicad_pcb: outline, island/bend zones, keep-outs.

Footprints are not placed here. Open the board and press F8 (Update PCB from
Schematic) to bring them in with their nets, then drag them onto the islands
marked below. On a flex board placement is a judgement call -- bend zones,
part orientation, strain relief -- and a script would only produce something
that passes DRC and fails in the fold.
"""
import re, uuid

REF = "/Library/Application Support/kicad/demos/sonde xilinx/sonde xilinx.kicad_pcb"

def blk(s, i):
    d, j, q = 0, i, False
    while j < len(s):
        c = s[j]
        if q:
            if c == '\\': j += 2; continue
            if c == '"': q = False
        elif c == '"': q = True
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: return s[i:j+1]
        j += 1

t = open(REF, errors="ignore").read()
layers = blk(t, t.index("(layers"))
setup  = blk(t, t.index("(setup"))
U = lambda: str(uuid.uuid4())

X0, Y0, L, W = 50.0, 60.0, 65.0, 10.0

def line(x1, y1, x2, y2, layer, width=0.1):
    return ('\n\t(gr_line\n\t\t(start %.3f %.3f)\n\t\t(end %.3f %.3f)'
            '\n\t\t(stroke\n\t\t\t(width %.3f)\n\t\t\t(type solid)\n\t\t)'
            '\n\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)' % (x1, y1, x2, y2, width, layer, U()))

def text(s, x, y, layer, size=0.9):
    return ('\n\t(gr_text "%s"\n\t\t(at %.3f %.3f 0)\n\t\t(layer "%s")\n\t\t(uuid "%s")'
            '\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %.2f %.2f)\n\t\t\t\t(thickness 0.15)\n\t\t\t)'
            '\n\t\t\t(justify left)\n\t\t)\n\t)' % (s, x, y, layer, U(), size, size))

g = []
for a, b_ in ((0, L), ):
    g += [line(X0, Y0, X0+L, Y0, "Edge.Cuts"),
          line(X0+L, Y0, X0+L, Y0+W, "Edge.Cuts"),
          line(X0+L, Y0+W, X0, Y0+W, "Edge.Cuts"),
          line(X0, Y0+W, X0, Y0, "Edge.Cuts")]

# The module goes flush against x=0 with its antenna facing the board edge,
# so the datasheet's 12.4 x 3.8 mm "no ground pad" area falls off the board
# and costs nothing. That is what makes the budget close.
zones = [
    (0.0,  15.5, "A: MODULE U1  (antenna at x=0, flush to the edge)"),
    (15.5, 19.0, "bend"),
    (19.0, 31.0, "B: POWER  U2 U3 U4 Q1 R3 R4 R5 C2..C6"),
    (31.0, 34.5, "bend"),
    (34.5, 50.5, "C: CELL BT1  16 x 8 mm"),
    (50.5, 54.0, "bend"),
    (54.0, 65.0, "D: MOTOR M1 + Q2 R1 R2 D1 C1, charge pads J1 J2"),
]
for a, b_, lab in zones:
    g.append(line(X0+a, Y0, X0+a, Y0+W, "Dwgs.User", 0.08))
    g.append(text(lab, X0+a+0.4, Y0+W+2.2, "Cmts.User", 0.7))
g.append(line(X0+L, Y0, X0+L, Y0+W, "Dwgs.User", 0.08))

# antenna no-copper region, on the board side of the module edge
g.append(line(X0, Y0, X0+3.8, Y0, "User.1", 0.15))
g.append(line(X0+3.8, Y0, X0+3.8, Y0+W, "User.1", 0.15))
g.append(line(X0+3.8, Y0+W, X0, Y0+W, "User.1", 0.15))

for i, s in enumerate([
    "ring-flex-v1   65.0 x 10.0 mm   2-layer polyimide   closed ring, index finger, 65 mm",
    "BEND ZONES: no vias, no pads, no component edges. Hatched pour only, traces perpendicular and curved.",
    "ANTENNA (User.1): no copper either layer. Datasheet keep-out is 12.4 x 3.8 mm; the board ends there.",
    "STIFFENERS under islands A, B, C and D. Static bend radius >= 6x stack thickness.",
    "Press F8 to bring footprints in from the schematic, then place them on the islands.",
]):
    g.append(text(s, X0, Y0 - 8.0 + i*2.0, "Cmts.User", 0.9))

out = ('(kicad_pcb\n\t(version 20241229)\n\t(generator "glucose-ring")\n\t(generator_version "9.0")'
       '\n\t(general\n\t\t(thickness 0.2)\n\t\t(legacy_teardrops no)\n\t)'
       '\n\t(paper "A4")\n\t%s\n\t%s\n\t(net 0 "")%s\n)\n'
       % (layers, setup, "".join(g)))
open("ring-flex-v1.kicad_pcb", "w").write(out)
print("board written: %.0f x %.0f mm, %d items, %d zones" % (L, W, len(g), len(zones)))
