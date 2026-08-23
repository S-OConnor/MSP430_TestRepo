# MSP430FR5969 ↔ ADC168M102R-SEP — 2-channel acquisition at ~100 Hz

Firmware for the MSP-EXP430FR5969 LaunchPad that drives the **ADC168M102R-SEP**
(16-bit, 8-channel, dual simultaneous-sampling SAR ADC) on TI's
**ADC168M102REVM-PDK** eval board and samples **CHA1 and CHB1** simultaneously
every tick (~99.9 Hz).

The two channels are a *pair* (CHA1 on converter A, CHB1 on converter B), so a
single conversion captures both at the same instant and a single readout
returns both — one ADC access per tick, ~20 µs of bus time. To acquire a
different pair, change `ADC_PAIR` in [src/board.h](src/board.h) (k → CHAk +
CHBk); nothing else needs editing.

**There is no serial output.** Results are read straight off the ADC bus with a
scope or logic analyzer: SDOA carries both 16-bit results in the 40-clock
readout burst that ends every tick. Trigger on the CONVST rising edge — it
fires once per 10 ms with quiet either side, so a single-shot capture lands on
a whole acquisition every time. Two LEDs report health, and every value the
firmware computes stays in a global a debugger can read.

Documentation:
- [docs/DESIGN.md](docs/DESIGN.md) — the full design: overview and key
  decisions, then the ADC/SPI background from first principles, the
  implementation in detail, and failure handling/troubleshooting.
- [docs/PLAN.md](docs/PLAN.md) — phased implementation plan and risk register.
- [EXAMPLE_OUTPUTS.md](EXAMPLE_OUTPUTS.md) — annotated bus timing for the CONFIG
  readback and for an acquisition, plus failure signatures.
- Reference PDFs (ADC datasheet SBASAW9, EVM guide SBAU478, MCU datasheet,
  LaunchPad guide) are in [docs/](docs/).

## How it works (short version)

The ADC's CLOCK pin is both conversion clock and serial clock; CONVST starts a
conversion, RD triggers readout. The datasheet permits a **gated burst clock**,
so eUSCI_B0 SPI (8 MHz, CPOL=0/CPHA=1) supplies clock bursts and shifts data
while CONVST/RD/~CS are GPIO strobes. Mode II + special read (M0 strapped low,
M1 pulled high, SR=1): each conversion converts one pair (CHAk + CHBk
simultaneously), and one RD pulse + 40 clocks reads both results on SDOA.
`PDE=1` puts both input muxes in the **pseudo-differential 4:1 configuration**
(datasheet Table 6-2), where CONFIG `C[1:0]` picks one of CHx0..CHx3 per
converter against the common mode; `M0=0` keeps that selection manual, so the
SEQFIFO sequencer (automatic mode only) stays at its reset default.
One conversion per tick (~20 µs) covers both channels; the mux selection is a
compile-time constant, so there is no channel rotation to keep in phase.
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

### Analog inputs

The two sampled channels arrive on the EVM's op-amp input headers (odd pins are
signals, even pins GND):

| ADC channel | EVM header | Where it appears on SDOA |
|---|---|---|
| CHA1 | **J2 pin 5** | frame A — first 20 clocks of the readout |
| CHB1 | **J1 pin 5** | frame B — second 20 clocks |

Both go through the OPA4H014-SEP buffers, which need the ±8 V rails on J3/J4.
The other six channel inputs (J2.1/3/7, J1.1/3/7) are unused and may be left
open. Jumpers JP1/JP2 stay in their default CMx_EXT position — the common mode
comes from the ADC's internal reference via the REFCM register, in firmware.

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

The image bundles TI msp430-gcc 9.3.1.11 + support files, CMake/Ninja and
mspdebug. For flashing from inside the container, pass the LaunchPad through:
`--device=/dev/bus/usb --device=/dev/ttyACM0`.

**Option B — native (Linux):**

1. TI **MSP430-GCC-OPENSOURCE** ([download page](https://www.ti.com/tool/MSP430-GCC-OPENSOURCE)):
   extract `msp430-gcc-9.3.1.11_linux64.tar.bz2` and
   `msp430-gcc-support-files-1.212.zip` under `~/ti/`, then
   `ln -s msp430-gcc-9.3.1.11_linux64 ~/ti/msp430-gcc`.
   (Different paths: `-DMSP430_GCC_ROOT=`/`-DMSP430_SUPPORT=` at configure time.)
2. `sudo dnf install cmake mspdebug` (or distro equivalent).
3. udev (once): the eZ-FET is a TI USB device (2047:0013); add your user to
   `dialout`/`plugdev` or an appropriate udev rule so `/dev/ttyACM*` and the
   HID debug interface are accessible without root.

## Build and flash

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/msp430-toolchain.cmake
cmake --build build                        # fw.elf + section sizes
cmake --build build --target flash         # mspdebug tilib (needs TI libmsp430.so)
cmake --build build --target flash-ezfet   # fallback: built-in eZ-FET driver
```

### Reading the results

Put the scope on **SDOA** (EVM J5.1) with **CONVST** (J5.13) as the trigger,
rising edge, single shot. Each tick produces one burst:

```
CONVST _|‾|______________________________________________
CLOCK  ____24 conversion clocks____40 readout clocks_____
BUSY   ___|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|______________________________
RD     _______________________|‾|________________________
SDOA   -------------------------[ frame A ][ frame B ]---
```

Each frame is 20 bits: `0`, an A/B indicator (0 = CHA1, 1 = CHB1), the 16-bit
two's-complement result MSB-first, then two zeros. Code → voltage is
2.5 V + code × 76.3 µV (0 V ≈ −32768, 2.5 V ≈ 0, 5 V ≈ +32767). See
[EXAMPLE_OUTPUTS.md](EXAMPLE_OUTPUTS.md) for annotated traces.

### LEDs and debugger state

| Signal | Meaning |
|---|---|
| LED1 (red, P1.0) blinking 1 Hz | ticking at the right rate — also a free 1 Hz scope reference |
| LED2 (green, P4.6) on | init failed, or a frame/BUSY error has occurred |

Halt with `mspdebug` and read the globals for detail: `g_status` (0x01 = no
external 32 kHz and running the DCO fallback tick, 0x02 = ADC link check
failed), `g_cfg` (raw CONFIG readback, expect `0x1041`), `g_sample_a`,
`g_sample_b`, `g_tick`, `g_err_frame`, `g_err_busy`.

## Bring-up ladder (matches docs/PLAN.md phases)

1. `make flash` a clean build → heartbeat LED (P1.0) blinks 1 Hz.
2. Scope on P2.2 (CLOCK) → a ~20 µs burst every 10 ms; `g_status` reads 0x0000
   with the 32 kHz source attached.
3. Wire the ADC per the table, power the EVM, reflash the normal build →
   error LED stays off, `g_cfg` reads `0x1041` (link check pass), and ~2.5 V
   appears on the EVM REFIO test points.
4. Feed known DC levels (0 / 2.5 / 5 V → ≈ −32768 / 0 / +32767) into J2.5
   (CHA1) and J1.5 (CHB1); *different* levels on the two confirm that frame A
   and frame B are not swapped and that the mux is on pair 1 — grounding J2.3
   (CHA2) should change nothing.
5. Soak: error LED stays off for minutes (`g_err_frame`/`g_err_busy` stay 0).

If frames come back shifted/corrupt at 8 MHz (see risk A in the plan), lower
`ADC_SCLK_DIV` in [src/board.h](src/board.h) to 2 (4 MHz) or 4 (2 MHz).
