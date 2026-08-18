# Architecture Overview (high level)

This document is the short "what and why" of the firmware. For a
ground-up explanation that assumes no SPI or ADC background, see
[DESIGN.md](DESIGN.md). For the phased build plan and risk register, see
[PLAN.md](PLAN.md).

## 1. What the system does

An MSP430FR5969 microcontroller (on a TI LaunchPad board) samples eight
analog voltages roughly 100 times per second using an external precision
ADC (TI ADC168M102R-SEP on its evaluation board), and prints each set of
readings as one line of comma-separated text over a USB serial port.

```
  8 analog inputs                                     PC serial terminal
  (0..5 V each)                                        (115200 baud)
        |                                                    ^
        v                                                    |
 +--------------+   SPI bus + 3 strobe wires   +-----------------+   USB (eZ-FET)
 | ADC168M102R  | <==========================> | MSP430FR5969    | ============>
 | (EVM board)  |   CLOCK, SDI, SDOA           | (LaunchPad)     |  CSV lines
 |              |   CONVST, RD, BUSY, ~CS      |                 |
 +--------------+                              +-----------------+
        ^                                              ^
        | +5 V, +3.3 V, +-8 V (bench supplies)         | 32.768 kHz square wave
                                                        (external timebase)
```

Output, one line per sample tick:

```
tick,a0,a1,a2,a3,b0,b1,b2,b3,errs
```

## 2. Key design decisions

| Decision | Choice | Why |
|---|---|---|
| ADC interface | eUSCI_B0 SPI at 8 MHz for CLOCK/SDI/SDOA + three GPIO strobes | The ADC's clock doubles as its conversion clock and the datasheet permits a gated ("burst") clock — which is exactly what an SPI master produces. 8 MHz is the closest DCO-derivable rate at or below the requested 10 MHz. |
| ADC operating mode | Mode II (M0 strapped low), special-read (SR=1), pseudo-differential (PDE=1) | One RD strobe + 40 clocks returns both converters' results; 4 pair conversions cover 8 channels; the internal 2.5 V reference is routed as common mode by software so the EVM needs no jumper changes. |
| Sample timebase | External 32.768 kHz square wave into LFXIN (bypass) → Timer_A0, period 328 → 99.902 Hz | User requirement (external low-frequency source). 100.000 Hz is not an integer division of 32768; 328 is the closest. |
| Fallback | If the 32 kHz source is missing: internal DCO timer at exactly 100 Hz, status flag set | Never hang; make the degraded state visible in the banner and on the error LED. |
| Output | Human-readable CSV, 115200 baud, interrupt-driven TX ring buffer | Readable in any terminal; ring buffer keeps the 10 ms tick path free of blocking I/O. |
| Integrity | Every ADC frame carries fixed indicator/zero bits which are checked on every read | Cheap, continuous self-test of wiring and clock phase; failures are counted in the `errs` column and light the error LED. |
| Toolchain | TI msp430-gcc via CMake cross file; Docker/Podman dev image | Reproducible builds on any host; no IDE dependency. |

## 3. Software structure

```
src/
├── board.h        pin map, tunables (SCLK divider, tick period), pin macros
├── clocks.c/.h    GPIO setup, LPM5 unlock, FRAM wait state, DCO/LFXT clocks
├── spi.c/.h       eUSCI_B0 SPI master, blocking byte exchange
├── uart.c/.h      eUSCI_A0 UART, 256-byte TX ring buffer + ISR
├── adc168m102.c/.h ADC driver: init sequence, per-pair conversion + readout
└── main.c         tick timer, scan loop, CSV formatting, LEDs
```

Layering (arrows = "uses"):

```
        main.c
       /  |   \
  uart.c  |   adc168m102.c
          |        |
       clocks.c   spi.c
          \        /
           board.h  (pin map + msp430.h device header)
```

## 4. Runtime behaviour

```
 power-on
   |
   v
 stop watchdog -> clocks (16 MHz CPU, 8 MHz SMCLK, 32 kHz ACLK)
   -> UART -> SPI -> ADC reset/config/link-check -> print banner
   -> start Timer_A0
   |
   v
 +--------------------------------------------------------------+
 |  sleep (LPM0)                                                |
 |     ^                                                        |
 |     |  timer ISR (~every 10 ms) sets flag, wakes CPU         |
 |     v                                                        |
 |  read pair 0,1,2,3   (each: CONVST -> 24 clocks -> BUSY low  |
 |                       -> RD -> 40 clocks -> parse + validate)|
 |  format CSV line -> enqueue in UART ring (non-blocking)      |
 |  update heartbeat / error LEDs                               |
 +--------------------------------------------------------------+
        UART ISR drains the ring in the background (~6 ms/line)
```

Per-tick budget: ~80 µs on the ADC bus, ~50 µs formatting, then the CPU
sleeps while the UART shifts the line out. CPU utilisation is ~2 %.

## 5. Failure handling

| Condition | Detection | Response |
|---|---|---|
| External 32 kHz absent | Oscillator fault flag never clears (bounded retry) | Switch tick timer to DCO, set `ST_NO_LFXT` (0x01), error LED |
| ADC not wired / unpowered / wrong strap | CONFIG readback mismatch at init | Set `ST_ADC_NOLINK` (0x02), banner shows raw readback, keep running |
| Conversion never completes | BUSY still high after timeout | Pair reported as −32768, `errs` incremented |
| Bit misalignment on the bus | Frame indicator/zero bits wrong | Pair reported as −32768, `errs` incremented |
| Host stops reading serial | UART ring full | Whole line dropped, counted in `errs`; sampling continues |

## 6. Where to look when something is wrong

- **Banner status ≠ 0x0000** → see table above; `cfg=` shows the raw ADC
  readback (expect `0x1041`).
- **Frames corrupt at 8 MHz** → lower `ADC_SCLK_DIV` in `board.h` (risk A
  in PLAN.md).
- **Columns map to wrong channels** → the next-pair command is pipelined;
  see the scan loop comment in `main.c`.
- **Rate slightly off 100 Hz** → intended (99.902 Hz); change
  `TICK_PERIOD_ACLK` if a different rate is preferred.
