# MSP430FR5969 ↔ ADC168M102R-SEP — 8-channel acquisition at ~100 Hz

Firmware for the MSP-EXP430FR5969 LaunchPad that drives the **ADC168M102R-SEP**
(16-bit, 8-channel, dual simultaneous-sampling SAR ADC) on TI's
**ADC168M102REVM-PDK** eval board, scans all 8 pseudo-differential channels
every tick (~99.9 Hz), and streams CSV over the ezFET backchannel UART at
115200 baud.

Documentation:
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — high-level overview, key
  decisions, module structure, failure handling.
- [docs/DESIGN.md](docs/DESIGN.md) — detailed design from first principles
  (assumes no ADC/SPI background).
- [docs/PLAN.md](docs/PLAN.md) — phased implementation plan and risk register.
- Reference PDFs (ADC datasheet SBASAW9, EVM guide SBAU478, MCU datasheet,
  LaunchPad guide) are in [docs/](docs/).

## How it works (short version)

The ADC's CLOCK pin is both conversion clock and serial clock; CONVST starts a
conversion, RD triggers readout. The datasheet permits a **gated burst clock**,
so eUSCI_B0 SPI (8 MHz, CPOL=0/CPHA=1) supplies clock bursts and shifts data
while CONVST/RD/~CS are GPIO strobes. Mode II + special read (M0 strapped low,
M1 pulled high, SR=1): each conversion converts one pair (CHAk + CHBk
simultaneously), and one RD pulse + 40 clocks reads both results on SDOA.
Four pipelined pair conversions per tick (~80 µs) cover all 8 channels.
The internal 2.5 V references are enabled at init and routed as the
pseudo-differential common mode (REFCM register) — no EVM jumper changes.
Input range per channel: 2.5 V ± 2.5 V → two's-complement codes
(0 V ≈ −32768, 2.5 V ≈ 0, 5 V ≈ +32767, LSB ≈ 76.3 µV).

## Wiring

LaunchPad → EVM header **J5** (odd pins are signals, even pins GND — connect at
least two grounds):

| LaunchPad | Dir | EVM J5 | ADC signal |
|---|---|---|---|
| P2.2 (UCB0CLK) | → | 7 | CLOCK |
| P1.6 (UCB0SIMO) | → | 15 | SDI |
| P1.7 (UCB0SOMI) | ← | 1 | SDOA |
| P1.4 | → | 9 | ~CS (held low in operation) |
| P2.6 | → | 13 | CONVST |
| P4.2 | → | 11 | RD |
| P1.5 | ← | 5 | BUSY |
| GND | — | even pins | DGND |
| **strap** | | **17 → GND** | **M0 = 0** (M1 stays pulled high → Mode II) |

SDOB (J5.3) and M1 (J5.19) stay unconnected.

### EVM power (PHI controller board **removed** — it contends on the digital lines)

- DVDD 3.3 V: remove **R19**, feed LaunchPad 3V3 → **TP3**.
- AVDD 5 V: remove **R34**, feed external 5 V → **TP2**. (3.3 V works for
  digital bring-up at reduced input range.)
- OPA_V+ ≈ +8 V / OPA_V− ≈ −8 V on **J3/J4** for the input amplifiers
  (required for real analog measurements, not for digital bring-up).
- JP1/JP2 stay in the default CMx_EXT position (common mode is internal via
  firmware).
- Common ground between LaunchPad, EVM, and all supplies.

### 32.768 kHz timebase

An external 32.768 kHz **square wave (0–3.3 V)** feeds LFXIN (PJ.4) in bypass
mode. On the LaunchPad, PJ.4 carries the onboard crystal Y1 and is not on a
header — attach at the crystal pad and ideally remove Y1. Sample rate is
32768/328 = **99.902 Hz**. If the source is missing, the firmware reports it
(status bit 0x01, error LED) and falls back to a DCO-derived 100 Hz tick so
streaming continues.

## Toolchain setup

**Option A — Docker (recommended, reproducible):**

```sh
docker build -t msp430-dev .
docker run --rm -it -v "$PWD":/work msp430-dev
```

(`podman` works with the same commands; on SELinux hosts mount with
`-v "$PWD":/work:z`.)

The image bundles TI msp430-gcc 9.3.1.11 + support files, CMake/Ninja,
mspdebug, and picocom. For flashing/serial from inside the container, pass the
LaunchPad through: `--device=/dev/bus/usb --device=/dev/ttyACM0 --device=/dev/ttyACM1`.

**Option B — native (Linux):**

1. TI **MSP430-GCC-OPENSOURCE** ([download page](https://www.ti.com/tool/MSP430-GCC-OPENSOURCE)):
   extract `msp430-gcc-9.3.1.11_linux64.tar.bz2` and
   `msp430-gcc-support-files-1.212.zip` under `~/ti/`, then
   `ln -s msp430-gcc-9.3.1.11_linux64 ~/ti/msp430-gcc`.
   (Different paths: `-DMSP430_GCC_ROOT=`/`-DMSP430_SUPPORT=` at configure time.)
2. `sudo dnf install cmake mspdebug picocom` (or distro equivalent).
3. udev (once): the eZ-FET is a TI USB device (2047:0013); add your user to
   `dialout`/`plugdev` or an appropriate udev rule so `/dev/ttyACM*` and the
   HID debug interface are accessible without root.

## Build, flash, watch

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/msp430-toolchain.cmake
cmake --build build                        # fw.elf + section sizes
cmake --build build --target flash         # mspdebug tilib (needs TI libmsp430.so)
cmake --build build --target flash-ezfet   # fallback: built-in eZ-FET driver
cmake --build build --target term          # picocom on /dev/ttyACM1
```

Output format (`#` lines are comments):

```
# adc168m102 fw v0.1 status=0x0000 cfg=0x1041
# tick,a0,a1,a2,a3,b0,b1,b2,b3,errs
1,-16234,3,1023,-508,12,900,-3,88,0
```

`tick` increments at ~99.9 Hz; `a0..a3` = CHA0..CHA3, `b0..b3` = CHB0..CHB3
(signed 16-bit); `errs` = cumulative frame/BUSY/UART-drop errors (0 in a
healthy run). Status bits: 0x01 = no external 32 kHz (fallback tick),
0x02 = ADC link check failed (check wiring/M0 strap/power).

## Bring-up ladder (matches docs/PLAN.md phases)

1. `make flash` a clean build → heartbeat LED (P1.0) blinks 1 Hz.
2. `make term` → banner + CSV lines; status must be 0x0000 with the 32 kHz
   source attached.
3. SPI transport check without the ADC: jumper P1.6↔P1.7, reconfigure with
   `-DSPI_LOOPBACK_TEST=ON`, rebuild and flash → `# SPI loopback PASS`, slow
   LED blink. (Switch back with `-DSPI_LOOPBACK_TEST=OFF`.)
4. Wire the ADC per the table, power the EVM, reflash the normal build →
   banner `cfg=0x1041` (link check pass) and ~2.5 V on EVM REFIO test points.
5. Feed known DC levels (0 / 2.5 / 5 V → ≈ −32768 / 0 / +32767); distinct
   levels on different channels verify column↔channel mapping.
6. Soak: `errs` stays 0 for minutes.

If frames come back shifted/corrupt at 8 MHz (see risk A in the plan), lower
`ADC_SCLK_DIV` in [src/board.h](src/board.h) to 2 (4 MHz) or 4 (2 MHz).
