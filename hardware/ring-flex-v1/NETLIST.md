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
| U3 | TPS7A0230 (or XC6220) | SOT-23-5 | 3.0 V LDO for the motor rail, **EN pin required** |
| U4 | DW01A | SOT-23-6 | cell protection controller |
| Q1 | FS8205A | SOT-23-6 | dual N-FET, protection switch |
| Q2 | AO3400A (or DMG2302U) | SOT-23 | motor low-side switch |
| M1 | coin ERM, 10 x 2.1 mm, ~1.1 G | — | haptic actuator |
| D1 | BAT54W | SOD-323 | motor flyback |
| D2 | BAT54W | SOD-323 | reverse-polarity guard on the charge contact |
| R1 | 10 k, 0402 | — | **gate pulldown — mandatory** |
| R2 | 1 k, 0402 | — | gate series, optional |
| R3 | 66.5 k, 0402 | — | MCP73831 PROG: sets 15 mA |
| R4 | 330 R, 0402 | — | DW01A supply, per app circuit |
| C1 | 100 uF X5R 1206 | — | bulk on the motor rail |
| C2 | 10 uF X5R 0805 | — | VBAT bulk |
| C3 | 4.7 uF 0603 | — | charger output |
| C4 | 1 uF 0402 | — | charger input |
| C5 | 0.1 uF 0402 | — | DW01A |
| C6 | 1 uF 0402 | — | LDO output |
| BT1 | LiPo, ~30 mAh, 16 x 8 x 2.5 mm | — | bare cell (protection is on-board) |
| J1,J2 | gold pads | — | charge contacts for a pogo dock |
| TP1-4 | test pads | — | SWD |

## Nets

### Power

| Net | Connects |
|---|---|
| `CELL+` | BT1 + ; U4 VDD via R4 ; `VBAT` |
| `CELL-` | BT1 - ; Q1 drain pair (protection switch) |
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
| `VCHG` | J1 ; D2 anode |
| `VCHG_IN` | D2 cathode ; U2 VDD ; C4 |
| `CHG_STAT` | U2 STAT (open drain) ; U1 **P0.17** |
| `PROG` | U2 PROG ; R3 to `GND` |

`R3` = 66.5 k gives **15 mA**, the MCP73831's floor. Size the cell at 30 mAh
or more so this stays at or under 0.5C. If a smaller cell is wanted, use the
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
