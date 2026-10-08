# ring-flex-v1 — a flexible-PCB build of the glucose ring

The POC works: a XIAO nRF52840 hand-wired to a coin motor, relaying Nightscout
readings over BLE and buzzing threshold alerts. Verified end to end on
hardware (IOS_APP_REPORT.md section 15). This directory is the custom board
that replaces it.

**Status: design, nothing fabricated.**

## Target

| | |
|---|---|
| Finger circumference | **65 mm** (20.7 mm inner diameter, about a US size 11) |
| Band thickness | **~3.5 mm**, set by the cell |
| Runtime | **6-9 days** on ~25-30 mAh at 50-150 uA idle |
| Charge | pogo contacts, 15 mA (<= 0.5C on a 30 mAh cell) |
| Firmware updates | BLE DFU after one SWD flash; no USB connector |

## The decisions, and why

**Raytac MDBT50Q-1MV2, not a bare nRF52840.** A bare QFN means designing a
matching network and antenna from scratch and debugging RF with no reference.
The module is pre-certified, castellated (hand-solderable), and 10.2 x 15.6 mm
against the XIAO's 21 x 17.5 mm.

**10 x 2.1 mm coin ERM, not the 10 x 3.4 mm one.** ~1.1 G against ~1.2-1.4 G
-- a small strength trade for 1.3 mm of thickness, and it keeps the drive
circuit and the buzz patterns that were already proven distinguishable on a
finger. An LRA would be stronger at a better frequency (210 Hz is near peak
skin sensitivity) with sharper edges, but needs a DRV2605L, an I2C bus and
retuned patterns. That is the v2 upgrade, after the board itself works.

**The MCU runs straight from the cell; only the motor is regulated.** The
nRF52840 takes 1.7-3.6 V with its internal DC/DC. The motor is the 3 V part,
so it gets the LDO. This also removes the awkwardness on the POC where the
3300 mV cutoff sat close to the regulator's dropout.

**The motor rail LDO is gated by a GPIO.** `MOTOR_EN` enables it only for a
buzz, so it draws nothing the rest of the time. On a 30 mAh cell, quiescent
current is not a rounding error.

**Charge current is ours to choose.** The whole 40 mAh-cell problem on the POC
came from the XIAO's charger being fixed at 50 mA. Here R3 sets it: 15 mA.

**No battery divider.** With the MCU on the cell, the SAADC reads VDD
internally -- deleting two resistors, the divider switch, the `VBAT_ENABLE`
pin, the pulsed-sampling logic, and a calibration that was never closed.

## Band layout, 65 mm

```
 [ A: module 15.5 ]-3.5-[ B: power 12 ]-3.5-[ C: cell 16 ]-3.5-[ D: motor 11 ]
 0              15.5      19      31       34.5      50.5     54        65
```

- **Antenna at the outside of the ring**, furthest from the finger. Skin is
  saltwater and absorbs.
- **Cell diametrically opposite the antenna.** A metal pouch beside a chip
  antenna detunes it.
- **Antenna keep-out: no copper, no cell, no motor within 5-10 mm.**

## Flex rules that actually bite

- Nothing in a bend zone: no vias, no pads, no component edges.
- Hatched copper pour in flex areas, never solid -- solid stiffens and cracks.
- Traces perpendicular to the bend, curved, no 90 degree corners. Teardrop
  every pad.
- Static bend radius >= 6x stack thickness. A ring is a static bend.
- Stiffeners under the module, the cell and the motor.
- 2-layer polyimide, ENIG, coverlay. 4-layer costs money and flexibility.

## Firmware deltas

Pin assignments were chosen to keep the diff small: the motor stays on
**P1.15** and charge detect stays on **P0.17**, so `MOTOR_PIN` and
`PIN_CHG_STATUS` do not change at all.

| Change | Why |
|---|---|
| add `MOTOR_EN` (P1.13), raise around a buzz | the LDO is gated now |
| `readVbatMv()` -> SAADC internal VDD | no external divider |
| drop `VBAT_DIVIDER_COMP`, `VBAT_ENABLE` | gone with the divider |
| `chargerActive()`: drop the VBUS term | no USB, so `sd_power_usbregstatus_get` always reads 0 |
| bootloader board variant | the Adafruit nRF52 bootloader is board-specific |

Everything else ports untouched: the protocol, the liveness logic, the alert
patterns, the MTU chunking fix, the 450 host tests.

## From the datasheet (Version H, read 2026-10-07)

Confirmed against Raytac's approval sheet for the MDBT50Q-1MV2:

| | |
|---|---|
| Body | **15.5 x 10.5 x 2.05 mm** |
| Pads | **61, each 0.6 x 0.4 mm** |
| Antenna keep-out | **12.4 x 3.8 mm of "no ground pad"** beyond the antenna end |
| Extra | a 1.6 x 1.2 mm top-layer no-ground-pad area, 2.95 mm in from the left edge |

**The keep-out costs nothing if the module sits flush at the end.** Put U1
hard against x=0 with its antenna facing the board edge, and the 12.4 x 3.8 mm
"no ground pad" area falls off the board -- there is no host copper there
because there is no board there. That is what makes the budget close:

```
 [module 15.5][power 12][cell 16][motor 11] = 54.5 mm
 + 3 bends at 3.5 mm = 65.0 mm exactly
```

The first 3.8 mm of board under the module's antenna end is still marked as
a no-copper region on User.1, because the module overhangs it.

**The footprint is not reconstructed here.** The approval sheet gives pad
size and the keep-out but not per-pad pitch, and rebuilding 61 pad positions
off a drawing is exactly the sort of plausible-looking error that reaches
copper. Use Raytac's own footprint from their support page, or SnapMagic's,
and check it against the four numbers above before committing to fab.

## Open questions

- **Cell sourcing.** 16 x 8 x 2.5 mm at ~30 mAh with UN38.3 paperwork. Cheap
  cells without test reports become a redesign at certification.
- **Encapsulation.** Skin contact all day means ISO 10993 data on whatever it
  is potted in.
- **Dock mechanics.** Pogo alignment on a curved surface is its own problem.
- **The bootloader variant** is the largest unknown piece of work here.
