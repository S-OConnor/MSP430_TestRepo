# Architecture Overview (high level)

This document is the short "what and why" of the firmware. For a
ground-up explanation that assumes no SPI or ADC background, see
[DESIGN.md](DESIGN.md). For the phased build plan and risk register, see
[PLAN.md](PLAN.md).

## 1. What the system does

An MSP430FR5969 microcontroller (on a TI LaunchPad board) drives an external
precision ADC (TI ADC168M102R-SEP on its evaluation board) to sample two
analog voltages roughly 100 times per second.

The two inputs are **CHA1** (EVM header J2.5) and **CHB1** (J1.5). They sit
on the ADC's two independent converters at the same mux position, so one
conversion digitizes both *at the same instant* and one readout returns
both. Which pair is acquired is the `ADC_PAIR` constant in `board.h`.

**There is no output path off the MCU.** The results are read directly off
the ADC bus with a scope or logic analyzer — SDOA carries both 16-bit results
during the 40-clock readout burst that follows every tick. The firmware's only
job is to run that bus traffic reliably at 99.902 Hz and to flag trouble on
two LEDs.

```
  2 analog inputs                                    32.768 kHz square wave
  (0..5 V each)                                        (external timebase)
        |                                                    |
        v                                                    v
 +--------------+   SPI bus + 3 strobe wires   +-----------------+
 | ADC168M102R  | <==========================> | MSP430FR5969    |--> LED1 heartbeat
 | (EVM board)  |   CLOCK, SDI, SDOA           | (LaunchPad)     |--> LED2 error
 |              |   CONVST, RD, BUSY, ~CS      |                 |
 +--------------+          ^                   +-----------------+
        ^                  |                           ^
        | +5 V, +3.3 V,    |                           | USB: power + flash
        | +-8 V (bench)    +-- scope probes here:          (mspdebug)
                               SDOA carries both results
                               once per tick
```

## 2. Key design decisions

| Decision | Choice | Why |
|---|---|---|
| ADC interface | eUSCI_B0 SPI at 8 MHz for CLOCK/SDI/SDOA + three GPIO strobes | The ADC's clock doubles as its conversion clock and the datasheet permits a gated ("burst") clock — which is exactly what an SPI master produces. 8 MHz is the closest DCO-derivable rate at or below the requested 10 MHz. |
| ADC operating mode | Mode II (M0 strapped low), special-read (SR=1), pseudo-differential (PDE=1) | One RD strobe + 40 clocks returns both converters' results — exactly the two channels wanted — so a tick is a single conversion. The internal 2.5 V reference is routed as common mode by software, so the EVM needs no jumper changes. |
| Input mux | Pseudo-differential **4:1** configuration (`PDE=1`, datasheet Table 6-2), channel picked by CONFIG `C[1:0]` | Gives four single-ended inputs per converter measured against a common mode, which is what this application wants. `M0 = 0` keeps selection *manual* through `C[1:0]`; the SEQFIFO sequencer applies only to automatic mode (`M0 = 1`) and is left at its reset default. |
| Channel selection | Fixed pair, `ADC_PAIR = 1` → `C = 01` → CHA1 + CHB1, set in the init CONFIG word and re-asserted on every access | Picking two channels that share a mux position makes them simultaneous by construction and removes the pipelined channel-rotation entirely: the C field is a constant, so a corrupted command can only mis-select for one sample before the next access corrects it. |
| Sample timebase | External 32.768 kHz square wave into LFXIN (bypass) → Timer_A0, period 328 → 99.902 Hz | User requirement (external low-frequency source). 100.000 Hz is not an integer division of 32768; 328 is the closest. |
| Fallback | If the 32 kHz source is missing: internal DCO timer at exactly 100 Hz, status flag set | Never hang; make the degraded state visible on the error LED and in `g_status`. |
| Readout | None from the MCU — the ADC bus itself is the measurement point | The scope has to be on the bus during bring-up anyway, and SDOA already carries both results in full 16-bit resolution. Dropping the UART removes a peripheral, an ISR, a 256-byte buffer and a whole class of "did the host keep up?" failure from the tick path. |
| Status reporting | Two LEDs, plus every computed value held in a `volatile` global for the debugger | Enough to tell "alive and ticking" from "something is wrong" at a glance; `mspdebug` supplies the detail when the LED says to look. |
| Integrity | Every ADC frame carries fixed indicator/zero bits which are checked on every read | Cheap, continuous self-test of wiring and clock phase; failures are counted in the `errs` column and light the error LED. |
| Toolchain | TI msp430-gcc via CMake cross file; Docker/Podman dev image | Reproducible builds on any host; no IDE dependency. |

## 3. Software structure

```
src/
├── board.h        pin map, tunables (channel pair, SCLK divider, tick
│                 period), pin macros
├── clocks.c/.h    GPIO setup, LPM5 unlock, FRAM wait state, DCO/LFXT clocks
├── spi.c/.h       eUSCI_B0 SPI master, blocking byte exchange
├── adc168m102.c/.h ADC driver: init sequence, conversion + readout
└── main.c         tick timer, sample loop, LEDs, observable globals
```

Layering (arrows = "uses"):

```
          main.c
         /      \
   clocks.c    adc168m102.c
        \           |
         \        spi.c
          \        /
           board.h  (pin map + msp430.h device header)
```

## 4. Runtime behaviour

```
 power-on
   |
   v
 stop watchdog -> clocks (16 MHz CPU, 8 MHz SMCLK, 32 kHz ACLK)
   -> SPI -> ADC reset/config/link-check -> error LED if it failed
   -> start Timer_A0
   |
   v
 +--------------------------------------------------------------+
 |  sleep (LPM0)                                                |
 |     ^                                                        |
 |     |  timer ISR (~every 10 ms) sets flag, wakes CPU         |
 |     v                                                        |
 |  read pair 1         (CONVST -> 24 clocks -> BUSY low        |
 |                       -> RD -> 40 clocks -> parse + validate)|
 |  publish results into the observable globals                 |
 |  update heartbeat / error LEDs                               |
 +--------------------------------------------------------------+
        the scope sees one ~20 us burst per tick, 10 ms apart
```

Per-tick budget: ~20 µs on the ADC bus plus a few µs of bookkeeping, then the
CPU sleeps. CPU utilisation is well under 1 %.

## 5. Failure handling

| Condition | Detection | Response |
|---|---|---|
| External 32 kHz absent | Oscillator fault flag never clears (bounded retry) | Switch tick timer to DCO, set `ST_NO_LFXT` (0x01), error LED |
| ADC not wired / unpowered / wrong strap | CONFIG readback mismatch at init | Set `ST_ADC_NOLINK` (0x02) and light the error LED *before the first tick*; raw readback kept in `g_cfg`; keep running |
| Conversion never completes | BUSY still high after timeout | Both channels set to −32768, `g_err_busy` incremented, error LED latched |
| Bit misalignment on the bus | Frame indicator/zero bits wrong | Both channels set to −32768, `g_err_frame` incremented, error LED latched |

## 6. Where to look when something is wrong

The error LED (green, P4.6) is the only "something is wrong" signal. To find
out *what*, halt the target and read the globals — `mspdebug` then
`md &g_status`, `g_cfg`, `g_err_frame`, `g_err_busy`, `g_sample_a/b`, `g_tick`.

- **Error LED on from power-up** → `g_status`: 0x01 = no external 32 kHz
  (running on the DCO fallback tick), 0x02 = ADC link check failed. `g_cfg`
  holds the raw CONFIG readback (expect `0x1041`).
- **Heartbeat LED not blinking at 1 Hz** → the tick is not running; check the
  32 kHz source and that the ADC read is not timing out every tick.
- **Frames corrupt at 8 MHz** → lower `ADC_SCLK_DIV` in `board.h` (risk A
  in PLAN.md).
- **Wrong channel showing up** → check `ADC_PAIR` in `board.h` and the analog
  wiring (CHA1 = EVM J2.5 = frame A, CHB1 = EVM J1.5 = frame B). The mux
  selection is constant, so a rotation phase error is not possible here.
- **Rate slightly off 100 Hz** → intended (99.902 Hz); change
  `TICK_PERIOD_ACLK` if a different rate is preferred.
