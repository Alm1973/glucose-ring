# ring-flex-v1 — connection list

Every net, pin by pin. This is the schematic's content; capture in KiCad is
transcription against this.

**Module pins are given by signal name, not pin number.** The MDBT50Q-1MV2's
numbering must be read off its datasheet during capture — inventing it here
would be a plausible-looking error that survives review.

## Reference designators

| Ref | Part | Package | Function |
|---|---|---|---|
| U1 | Raytac MDBT50Q-1MV2 | module, castellated | nRF52840 + chip antenna |
| U2 | MCP73831T-2ACI/OT | SOT-23-5 | Li-ion charger, 4.2 V |
| U3 | **TPS7A0230PDBVR** | SOT-23-5 | 3.0 V, 200 mA, 25 nA Iq. EN has an internal pulldown, so the rail is off until firmware enables it. |
| U4 | DW01A | SOT-23-6 | cell protection controller |
| Q1 | FS8205A | SOT-23-6 | dual N-FET, **common drain**. Pin-1 dot beside S1: 1=S1 2=D1/D2 3=S2 4=G2 5=D1/D2 6=G1. |
| Q2 | AO3400A (or DMG2302U) | SOT-23 | motor low-side switch |
| M1 | coin ERM, 10 x 2.1 mm, ~1.1 G | — | haptic actuator |
| D1 | BAT54W | SOD-323 | motor flyback |
| D2 | BAT54W | SOD-323 | reverse-polarity guard on the charge contact |
| R1 | 10 k, 0402 | — | **gate pulldown — mandatory** |
| R2 | 1 k, 0402 | — | gate series, optional |
| R3 | 66.5 k 1%, 0402 | — | MCP73831 PROG. I = 1000/R(kΩ): 15 mA typ, 12.5–16.5 mA across the datasheet band. |
| R4 | **1 k**, 0402 | — | DW01A VCC feed (R1). The datasheet sets a 470 R minimum; 330 R was below it. |
| R5 | **2 k**, 0402 | — | DW01A CS to pack negative (R2). The datasheet states R1 and R2 cannot be omitted. |
| C1 | 100 uF X5R 1206 | — | bulk on the motor rail |
| C2 | 10 uF X5R 0805 | — | VBAT bulk |
| C3 | 4.7 uF 0603 | — | charger output |
| C4 | 1 uF 0402 | — | charger input |
| C5 | 0.1 uF 0402 | — | DW01A |
| C6 | 1 uF 0402 | — | LDO output |
| BT1 | LiPo, ~30 mAh, 16 x 8 x 2.5 mm | — | bare cell (protection is on-board) |
| J1,J2 | 2 x 2 mm gold pads | — | charge contacts for a pogo dock. ENIG, no paste. |
| J3 | USB-C receptacle, 16P | — | **not populated** — pads only, in case BLE DFU misbehaves |
| R6,R7 | 5.1 k, 0402 | — | USB-C CC pulldowns. Without them a USB-C source supplies nothing. |
| TP1-4 | test pads | — | SWD |

## Nets

### Power

| Net | Connects |
|---|---|
| `CELL+` | BT1 + ; U4 VCC via R4 ; `VBAT` |
| `CELL-` | BT1 - ; Q1 pin 1 (S1) ; U4 GND |
| `Q1_MID` | Q1 pins 2 and 5 — the FS8205A's shared D1/D2 |
| `DW_CS` | U4 CS ; R5 to `GND` |
| `VBAT` | `CELL+` ; U1 VDD ; U2 VBAT ; U3 VIN ; C2 ; C3 |
| `GND` | Q1 source ; U1 GND ; U2 VSS ; U3 GND ; U4 GND ; Q2 source ; C2,C3,C4,C5,C6,C1 ; J2 ; TP4 |
| `V_MOTOR` | U3 VOUT ; M1 + ; D1 cathode ; C1 ; C6 |

The MCU runs straight from the cell (nRF52840: 1.7-3.6 V, internal DC/DC).
Only the motor is regulated, because the motor is the 3 V part.

### Motor

| Net | Connects |
|---|---|
| `V_MOTOR` | M1 red (+) |
| `MOTOR_SW` | M1 blue (-) ; Q2 drain ; D1 anode |
| `MOTOR_GATE` | Q2 gate ; R1 ; R2 |
| `MOTOR_PWM` | U1 **P1.15** ; R2 (or straight to Q2 gate if R2 omitted) |
| `MOTOR_EN` | U1 **P1.13** ; U3 EN |

`R1` runs `MOTOR_GATE` to `GND`. **Not optional** — a floating gate turns the
motor on whenever the pin is high-Z, which is every reset and every second in
the bootloader.

`D1` cathode to `V_MOTOR`, anode to `MOTOR_SW`. Band toward the supply.

### Charging

| Net | Connects |
|---|---|
| `VCHG` | J1 ; D2 anode ; J3 VBUS (A4, A9, B4, B9) |
| `USB_CC1` | J3 A5 ; R6 to `GND` |
| `USB_CC2` | J3 B5 ; R7 to `GND` |
| `VCHG_IN` | D2 cathode ; U2 VDD ; C4 |
| `CHG_STAT` | U2 STAT (open drain) ; U1 **P0.17** |
| `PROG` | U2 PROG ; R3 to `GND` |

`R3` = 66.5 k gives **15 mA typical**, the MCP73831's floor (programmable
range 15–500 mA, R_PROG 2–67 kΩ). The datasheet's band at 67 kΩ is
12.5 / 15 / 16.5 mA, so size the cell for the **16.5 mA worst case** — on
30 mAh that is 0.55C. Anything above 67 kΩ falls into the shutdown zone. If a smaller cell is wanted, use the
BQ25100 instead, which reaches 10 mA -- at the cost of a DSBGA package.

`CHG_STAT` is read with the MCU's internal pull-up, exactly as the XIAO's
`~CHG` is today.

### Programming

| Net | Connects |
|---|---|
| `SWDIO` | U1 SWDIO ; TP1 |
| `SWCLK` | U1 SWCLK ; TP2 |
| `VBAT` | TP3 |
| `GND` | TP4 |

Flash the bootloader once over SWD; after that, firmware updates go over BLE
DFU. There is no USB connector on this board.

## Deliberately absent

- **No battery divider.** The MCU is powered from the cell, so the SAADC reads
  VDD internally. That removes two resistors, the divider switch, the
  `VBAT_ENABLE` pin, the pulsed-sampling logic, and the calibration that was
  never closed on the POC.
- **No USB.** Charging is by pogo contacts; updates are over BLE.
- **No LED.** `CHG_STAT` already reaches the MCU.
