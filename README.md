# MSP430FR5969 ↔ ADC168M102R-SEP — 2-channel acquisition at 100 Hz

Firmware for the MSP-EXP430FR5969 LaunchPad that drives the **ADC168M102R-SEP**
(16-bit, 8-channel, dual simultaneous-sampling SAR ADC) on TI's
**ADC168M102REVM-PDK** eval board and samples **CHA1 and CHB1** simultaneously
every tick (100.0 Hz).

The two channels are a *pair* (CHA1 on converter A, CHB1 on converter B), so a
single conversion captures both at the same instant; two read accesses then
fetch the two results — ~150 µs of bus time per tick at the 0.5 MHz SPI clock.
To acquire a different pair, change `ADC_PAIR` in [src/board.h](src/board.h)
(k → CHAk + CHBk); nothing else needs editing.

**Acquisition starts on a button press.** Out of reset the firmware sits in an
idle phase: once a second it writes the ADC's CONFIG register with the "read it
back" action and reads the reply — a link probe, no conversions, analog front
end untouched. Pressing **S1 (P4.5)** or **S2 (P1.1)** rewrites the operating
CONFIG and switches to continuous 100 Hz acquisition for good (reset to go
back). The heartbeat LED (green, P1.0) tells the phases apart: 0.5 Hz blink
idle, 1 Hz blink streaming. Both the idle period and the button debounce come
off the same 100 Hz tick — `IDLE_CONFIG_TICKS` / `BTN_DEBOUNCE_POLLS` in
[src/board.h](src/board.h). Full rationale and state machine:
[docs/DESIGN.md §4](docs/DESIGN.md#4-runtime-behaviour) and
[§10.4](docs/DESIGN.md#104-the-idle-probe-and-the-hand-over-to-streaming).

**There is no serial output.** Results are read straight off the ADC bus with a
scope or logic analyzer: SDOA carries both 16-bit results, one per read access,
in the two readout bursts that end every tick. Trigger on the CONVST rising
edge — it fires once per 10 ms with quiet either side, so a single-shot capture
lands on a whole acquisition every time. Two LEDs report health, and every value the
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
so eUSCI_B0 SPI (0.5 MHz, CPOL=0/CPHA=1) supplies clock bursts and shifts data
while CONVST/RD/~CS are GPIO strobes. Plain Mode II (M0 strapped low, M1
pulled high, **SR=0** — special read is not used): each conversion converts one
pair (CHAk + CHBk simultaneously), and because one read access carries one
20-bit frame on SDOA, the two results are fetched by two read accesses — RD
plus 24 clocks for converter A, then RD plus 24 clocks for converter B. SDOA is
the only data output either way; M1 pulled high leaves SDOB inactive.
`PDE=1` puts both input muxes in the **pseudo-differential 4:1 configuration**
(datasheet Table 6-2), where CONFIG `C[1:0]` picks one of CHx0..CHx3 per
converter against the common mode; `M0=0` keeps that selection manual, so the
SEQFIFO sequencer (automatic mode only) stays at its reset default.
One conversion per tick (~150 µs at 0.5 MHz) covers both channels; the mux
selection is a compile-time constant, so there is no channel rotation. The idle
phase uses exactly the same vocabulary — RD plus 24 clocks to write
`CONFIG = 0x1041`, RD plus 24 clocks to read the reply, once a second — which
is the nice consequence of dropping special read: every access on the bus is
one RD pulse and three bytes, whether it carries a register or half a
conversion. Because the probe word carries `C = 00`, the button press must
rewrite the operating word (`0x5040`) and flush two conversions before the
first sample is valid; without it the ADC would digitize pair 0 rather than
`ADC_PAIR`.
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

Nothing else needs wiring — the buttons and LEDs the firmware uses are on the
LaunchPad itself. The board's two user-interface clusters sit on opposite
ports (SLAU535B schematic, p. 37):

| LaunchPad control | Pin | Used for |
|---|---|---|
| S1 (left button) | P4.5 | start streaming (internal pull-up, active low) |
| S2 (right button) | P1.1 | start streaming — identical to S1 |
| LED1 (red) | P4.6 | error / degraded status, latched |
| LED2 (green) | P1.0 | heartbeat: 0.5 Hz idle, 1 Hz streaming |

### Analog inputs

The two sampled channels arrive on the EVM's op-amp input headers (odd pins are
signals, even pins GND):

| ADC channel | EVM header | Where it appears on SDOA |
|---|---|---|
| CHA1 | **J2 pin 5** | frame A — the first read access after BUSY falls |
| CHB1 | **J1 pin 5** | frame B — the second read access |

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

### Timebase

Nothing to wire, and **no crystal is used**. Every clock comes from the MCU's
internal DCO: MCLK (CPU) 16 MHz, SMCLK 8 MHz, and Timer_A0 from SMCLK/8 = 1 MHz
with a period of 10000 → an exact **100.000 Hz** sample tick, accurate to the
DCO's ~±2 %. That is all the tick needs to be: it spaces the ADC bursts evenly
and nothing measures absolute time from it.

LFXT and HFXT are both held off, so PJ.4/PJ.5 (LFXIN/LFXOUT) stay plain GPIO
and the LaunchPad's onboard 32.768 kHz crystal **Y4** sits idle — leave it
fitted or don't, the firmware never touches it. Two things follow: boot is
immediate (there is no ~1 s crystal start-up window to wait through), and there
is no crystal-failure mode to report, so status bit 0x01 is retired.

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
| LED2 (green, P1.0) blinking 0.5 Hz | idle phase — probing CONFIG once a second, waiting for a button |
| LED2 (green, P1.0) blinking 1 Hz | streaming, ticking at the right rate — also a free 1 Hz scope reference |
| LED1 (red, P4.6) on | init failed, or a config-probe/frame/BUSY error has occurred |

Halt with `mspdebug` and read the globals for detail: `g_status` (0x02 = ADC
link check failed; that is the only flag — 0x01 was the crystal-failure bit and
is retired), `g_phase` (0 = idle, 1 = streaming), `g_cfg` (raw CONFIG readback,
expect `0x1041`), `g_cfg_cycles` / `g_err_cfg` (idle probes done / failed),
`g_sample_a`, `g_sample_b`, `g_tick`, `g_err_frame`, `g_err_busy`.

## Bring-up ladder (matches docs/PLAN.md phases)

1. `make flash` a clean build → heartbeat LED (P1.0) blinks 0.5 Hz (idle);
   press S1 or S2 → the blink doubles to 1 Hz (streaming).
2. Scope on P2.2 (CLOCK) → idle shows two 24-clock probe bursts once a second
   (2 µs per clock at 0.5 MHz); after the button press, a ~150 µs acquisition
   — one 24-clock conversion burst then two 24-clock read accesses — every
   10 ms. `g_status` reads 0x0000 before the ADC is wired.
3. Wire the ADC per the table, power the EVM, reflash the normal build → in
   the idle phase the error LED stays off, `g_cfg` reads `0x1041` and
   `g_cfg_cycles` climbs about once a second (the link probe passing over and
   over), and ~2.5 V appears on the EVM REFIO test points. Pull a J5 wire and
   the error LED lights within a second — `g_err_cfg` starts counting.
4. Press S1 or S2 to start streaming, then feed known DC levels
   (0 / 2.5 / 5 V → ≈ −32768 / 0 / +32767) into J2.5 (CHA1) and J1.5 (CHB1);
   *different* levels on the two confirm that frame A and frame B are not
   swapped and that the mux is on pair 1 — grounding J2.3 (CHA2) should change
   nothing.
5. Soak: error LED stays off for minutes (`g_err_frame`/`g_err_busy`/`g_err_cfg`
   stay 0).

SCLK already runs at 0.5 MHz — the slowest rate the ADC accepts in half-clock
mode, which is the maximum margin available against risk A in the plan. If
frames still come back shifted/corrupt, the cause is not clock speed: check the
strobe wiring and the M0 strap. To trade that margin back for speed, raise the
rate in [src/board.h](src/board.h): `ADC_SCLK_DIV` 8 → 1 MHz, 4 → 2 MHz,
1 → 8 MHz.
