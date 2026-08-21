# MSP430FR5969 ↔ ADC168M102R-SEP: 2-channel acquisition at ~100 Hz — Phased Plan

## Context

Greenfield firmware in `/var/home/soconnor/repos/msp430` (empty except `docs/` reference PDFs: EVM guide SBAU478, FR5969 datasheet SLAS704G, LaunchPad guide SLAU535; ADC datasheet SBASAW9 to be added). An MSP430FR5969 LaunchPad drives the ADC168M102R-SEP on the ADC168M102REVM-PDK over jumper wires, configures the ADC, and reads **two** pseudo-differential channels each tick. There is no MCU-side output path: the results are observed directly on the ADC bus with a scope.

The ADC is **not a plain SPI peripheral**: CLOCK is both conversion and serial clock (half-clock 0.5–20 MHz), CONVST starts conversions, RD triggers readout. Datasheet §6.3.1.4 explicitly allows **burst/gated clocking** — so eUSCI_B0 SPI supplies clock bursts and shifts data, while CONVST/RD/CS are GPIO strobes. All register values and frame formats were verified against SBASAW9 (timing pp.9–11, modes pp.22–31, register map pp.32–41).

Agreed decisions:
- **Channels: CHA1 + CHB1** (`ADC_PAIR = 1`). The two converters share a mux position, so one conversion captures both simultaneously and one 40-clock readout returns both — a tick is a single ADC access (~20 µs). Analog inputs land on EVM **J2 pin 5** (CHA1) and **J1 pin 5** (CHB1).
- **SPI SCLK = 8 MHz** (10 MHz not synthesizable from the FR5969 DCO; ADC allows 20 MHz).
- **Timebase**: external 32.768 kHz square wave into LFXIN (LFXT **bypass**) → ACLK.
- **Rate**: Timer_A0 up mode on ACLK, CCR0=327 (period 328) → **99.902 Hz** (closest exact fit to 100 Hz; 2^15 has only power-of-2 divisors). 320→102.4 Hz is a one-constant alternative.
- **Readout**: none from the MCU. SDOA carries both 16-bit results every tick; a scope/logic analyzer on the bus is the measurement instrument, triggered on CONVST. Health is reported on the two LaunchPad LEDs, detail via `volatile` globals read with mspdebug.
- **Toolchain**: TI msp430-elf-gcc + Makefile, mspdebug/UniFlash. *Nothing installed on this machine yet.*

## Hardware reference (goes in README; user performs wiring)

LaunchPad → EVM header J5 (odd pins signal, even pins GND — connect ≥2 grounds):

| LaunchPad | Dir | EVM J5 | ADC signal |
|---|---|---|---|
| P2.2 UCB0CLK | → | 7 | CLOCK |
| P1.6 UCB0SIMO | → | 15 | SDI |
| P1.7 UCB0SOMI | ← | 1 | SDOA |
| P1.4 GPIO out | → | 9 | ~CS (held low) |
| P2.6 GPIO out | → | 13 | CONVST |
| P4.2 GPIO out | → | 11 | RD |
| P1.5 GPIO in (pulldown) | ← | 5 | BUSY |
| GND | — | even pins | DGND |
| strap | | 17 → GND | **M0=0** (M1 stays pulled high → Mode II, SDOA only) |

Analog inputs (EVM op-amp headers, odd pins signal / even pins GND): **CHA1 = J2 pin 5**, **CHB1 = J1 pin 5**, both via the OPA4H014-SEP buffers (need ±8 V on J3/J4). The other six channel inputs stay open.

EVM power (PHI controller **removed** — it would contend on the digital lines): DVDD 3.3 V = LaunchPad 3V3 → TP3 (remove R19); AVDD 5 V external → TP2 (remove R34); ±8 V on J3/J4 for input op-amps (needed for real analog readings, not digital bring-up); JP1/JP2 stay default (common mode comes from the internal reference via REFCM — firmware only). Common ground everywhere.

32.768 kHz source: PJ.4/LFXIN carries onboard crystal Y1 and isn't on a header — attach at the crystal pad, ideally remove Y1; 0–3.3 V swing. (Fallback: feed a TAxCLK header pin instead — small isolated change.)

## ADC operating configuration (fixed facts, encode in `adc168m102.h`)

Mode II + special read (M0=0, M1=1, SR=1): **manual** channel select (Table 6-5 — the SEQFIFO sequencer applies only to automatic mode, M0=1, and stays at its 0000h reset default, whose SL=00 field explicitly says "use mode I or II instead"), SDOA only; one RD pulse then 40 CLOCKs shift ADC-A frame then ADC-B frame. PDE=1 selects the **pseudo-differential 4:1 multiplexer configuration** (Table 6-2 / Table 7-2): C[1:0] picks CHx0..CHx3 per converter against "CMx / REFIOx". Pair k converts CHAk+CHBk simultaneously vs 2.5 V CM. We fix k = `ADC_PAIR` = 1, so both frames of that single readout are wanted data and no result is discarded. CID=0: each 20-bit frame = `0, ADC-indicator (0=A/1=B), 16-bit two's-complement result MSB-first, trailing 0s`.

CONFIG layout: C[15:14] R[13:12] PD[11:10] FE(9) SR(8) FC(7) PDE(6) CID(5) CE(4) A[3:0]. SPI: MSB first, **CPOL=0/CPHA=1** → eUSCI `UCCKPL=0, UCCKPH=0` (TI's UCCKPH is inverse of Motorola CPHA).

Words: reset `0x0004`; CONFIG `0x1140 | (ADC_PAIR<<14)` = **`0x5140`** (R=01, SR=1, PDE=1, CID=0, C=01 so the first conversion is already pair 1); REFDAC1 ptr `0x1142` / REFDAC2 ptr `0x1145` then `0x03FF` (RPD=0, 2.5 V); REFCM ptr `0x114C` then `0xFF00` (all channels armed with internal REFIO1 as CM — only two are read, but this keeps `ADC_PAIR` a one-line change). Link-check `0x1041` (read-CONFIG, done **before** SR=1 active); `g_cfg` holds this readback, not the operating word.

Gated-clock subtleties (handled by design):
1. Register updates activate on the CLOCK rising edge *after* the 16-clock write — `write_word()` always sends a **3rd dummy byte**.
2. SR/PDE/CID take effect one read access late — init ends with **two discarded conversion/read cycles**, which also re-command `ADC_PAIR`.
3. t2/t3 strobe-width spec (≤1 t_CLK) assumes a free-running clock; with gated clock no edges occur during the strobe. Bring-up risk A; mitigation is one define (`ADC_SCLK_DIV` → 2 or 4).

## Repo layout

> **Amendment (post-approval, per user):** build system is **CMake**
> (`CMakeLists.txt` + `cmake/msp430-toolchain.cmake` cross file) instead of a
> plain Makefile, plus a `Dockerfile` providing a reproducible dev container
> (toolchain + cmake + mspdebug). Flash becomes a CMake custom target.

```
msp430/
├── CMakeLists.txt
├── cmake/msp430-toolchain.cmake
├── Dockerfile
├── README.md                  # wiring, straps, power mods, toolchain install, usage
├── docs/                      # reference PDFs (present; add SBASAW9)
└── src/
    ├── board.h                # pin map macros, tunables (ADC_PAIR, ADC_SCLK_DIV, tick period)
    ├── main.c                 # tick loop, ISRs, observable globals, LEDs
    ├── clocks.c/.h            # clock_init(): LFXT bypass + fault fallback
    ├── spi.c/.h               # eUSCI_B0 master, spi_xfer()
    └── adc168m102.c/.h        # ADC driver
```
C99, direct registers via `<msp430.h>` + `-mmcu`, no driverlib/RTOS/printf (custom itoa).

---

# Phased implementation

Each phase is independently flashable and verified before the next begins; ADC hardware isn't touched until Phase 4.

## Phase 0 — Toolchain & skeleton (no hardware)

**Work**: Copy this plan into the repo as `docs/PLAN.md` (project-tracked copy). Install TI msp430-gcc tarball → `~/ti/msp430-gcc` (bundles `msp430fr5969` headers + linker scripts); `dnf install mspdebug`; ezFET udev note. Download SBASAW9 into `docs/`. Create repo layout, `README.md` (wiring table above), `Makefile` (`-mmcu=msp430fr5969 -mhwmult=f5series -Os -g3 -std=c99 -Wall -Wextra -ffunction-sections -fdata-sections`; `-I/-L` support dirs; targets `all`+size, `flash` via `mspdebug tilib` with `ezfet`/UniFlash `dslite.sh` fallbacks documented, `clean`), empty `main()`.
**Exit criteria**: `make` builds clean; `make flash` programs the LaunchPad.

## Phase 1 — Board bring-up: GPIO + clocks (`board.h`, `clocks.c`)

**Work**: WDT stop; full GPIO map (eUSCI pins **SEL1=1/SEL0=0**: `P1SEL1|=BIT6|BIT7`, `P2SEL1|=BIT0|BIT1|BIT2`; P1.5 input+pulldown; P1.4/P2.6/P4.2 outputs idle per table; LEDs P1.0/P4.6); `PM5CTL0 &= ~LOCKLPM5`; `FRCTL0 = FRCTLPW|NWAITS_1` **before** DCO→16 MHz (`CSCTL1 = DCOFSEL_4|DCORSEL`); `PJSEL0|=BIT4`; `CSCTL2 = SELA__LFXTCLK|SELS__DCOCLK|SELM__DCOCLK`; `CSCTL3 = DIVA__1|DIVS__2|DIVM__1` (SMCLK 8 MHz); `CSCTL4 = LFXTBYPASS`; bounded LFXTOFFG/OFIFG fault-clear loop → on persistent fault: ACLK=VLO + `ST_NO_LFXT` flag (no hang). Blink P1.0 from a software delay, then from an ACLK-driven timer.
**Exit criteria**: LED blinks at the expected period with the 32 kHz source attached (proves LFXT path); detaching the source flips to the fallback flag instead of hanging.

## Phase 2 — Sample tick (`main.c` skeleton)

**Work**: Timer_A0 up mode, ACLK, `TA0CCR0=327`, CCR0 ISR sets tick flag and exits **LPM0** (LPM0 rather than LPM3 because the no-LFXT fallback tick is clocked from SMCLK, which LPM3 stops). Fallback timer when `ST_NO_LFXT`: SMCLK/8, CCR0=9999 (8 MHz/8/10000 = exactly 100 Hz, DCO accuracy). Toggle the heartbeat LED every 50 ticks.
**Exit criteria**: heartbeat LED toggling on the tick; a scope on P1.0 measures the tick rate at ≈ 99.90 Hz (50 ticks per LED half-period).

## Phase 3 — SPI transport (`spi.c`)

**Work**: eUSCI_B0 master: `UCSWRST` → `UCMST|UCSYNC|UCMSB|UCSSEL__SMCLK` (UCCKPH=0/UCCKPL=0), `UCB0BRW=ADC_SCLK_DIV` (1 → 8 MHz) → release. Blocking `spi_xfer()` (TXIFG→TXBUF→RXIFG→RXBUF); eUSCI idles SCLK low between transfers = the ADC's permitted static-low burst clock.
**Exit criteria**: a scope on P2.2 (CLOCK) and P1.6 (SIMO) shows the expected byte on the wire at the configured `ADC_SCLK_DIV` rate.

## Phase 4 — ADC link + configuration (`adc168m102.c` part 1)

**Work**: strobe helpers `rd_pulse()`/`convst_pulse()` (two-instruction GPIO writes while SCLK static); `write_word(w)` = RD pulse + 3 SPI bytes (word MSB-first + activation dummy). `adc168_init()`: reset `0x0004` → link-check: write `0x1041`, RD pulse + 3-byte read, parse `((b0&0x3F)<<10)|(b1<<2)|(b2>>6)`, expect `0x104x` else `ST_ADC_NOLINK` → CONFIG `0x5140` → REFDAC1/2 `0x03FF` → REFCM `0xFF00` → 10 ms delay (t_REFON 8 ms) → two discarded conversion/read cycles. Latch the error LED if the link check failed.
**User prerequisite**: wiring per table, M0 strap, EVM powered (PHI removed).
**Exit criteria**: link-check passes (CONFIG readback matches); DMM shows ~2.5 V on EVM REFIO after init.

## Phase 5 — Conversions: the pair (`adc168m102.c` part 2)

**Work**: `adc168_read(*a, *b)`: CONVST pulse (never while BUSY high) → 3 dummy bytes (24 clocks ≥ 17.5+2 required) → poll BUSY low, ~50 µs timeout → RD pulse → 5-byte transfer, TX=`{ADC_PAIR<<6,0,0,0,0}` (R=00 "update C only", pipelined to the next conversion — always the same pair, so the selection is self-sustaining). Parse: `a=((b0&0x3F)<<10)|(b1<<2)|(b2>>6)`, `b=((b2&0x03)<<14)|(b3<<6)|(b4>>2)`; validate `(b0&0xC0)==0`, `(b2&0x3C)==0x04`, `(b4&0x03)==0` (relax to indicator bits only if the trailing-zero assumption fails on hardware). Print raw.
**Exit criteria**: indicator/zero validation passes every frame; sanity codes: grounded input ≈ −32768, 2.5 V ≈ 0, 5 V ≈ +32767. If frames are shifted/corrupt → risk A ladder: `ADC_SCLK_DIV=2` then `4`; last resort tie CONVST+RD (datasheet §8.2 four-wire mode).

## Phase 6 — Steady-state loop (`main.c` final)

**Work**: per tick: one `adc168_read(&va, &vb)` (~20 µs); on failure set both channels to INT16_MIN + bump the matching error counter; publish `g_sample_a`/`g_sample_b`/`g_tick` into `volatile` globals for the debugger; heartbeat LED 1 Hz, error LED latched on any nonzero counter or nonzero init status.
**Exit criteria**: distinct DC levels on EVM J2.5 (CHA1) and J1.5 (CHB1) appear in the right columns and are not transposed; grounding a neighbouring input (J2.3 = CHA2) changes nothing, confirming the mux sits on pair 1.

## Phase 7 — Soak & hardening

**Work**: minutes-long runs; verify errs stays 0 and rate stable; watch first-sample anomalies (datasheet f_DATA min 25 kSPS vs our burst/idle pattern — mitigation if seen: one dummy conversion per tick); optional logic-analyzer capture (expected: CONVST ↑↓ ~0.2 µs SCLK-static → BUSY ↑ → 24-clock burst, BUSY ↓ after 18th falling edge → RD ↑↓ → 40-clock burst; SDOA byte0=`00xxxxxx`, byte2 matches `xx0001xx`; SDI bytes 0–1 = the constant C word `0x40 0x00`). Record results + final wiring photos/notes in README.
**Exit criteria**: clean soak; README complete enough to rebuild the setup from scratch.

---

## Risk register

- **A. Gated-clock strobe width** (t2/t3 ≤1 t_CLK assumes free-running clock) → Phase 5 ladder: SCLK 4 MHz → 2 MHz → tied CONVST+RD mode.
- **B. Channel-rotation phase** (C word pipelined) → **retired** by the 2-channel design: C is a compile-time constant re-asserted on every access, so there is no rotation to fall out of phase. Init's `C = ADC_PAIR` plus the two discards prime it; a corrupted command word costs at most one sample. Phase 6's DC test still confirms the pair and the A/B ordering.
- **C. Register activation under gated clock** → 3rd dummy byte in every write; confirmed by Phase 4 link-check + REFIO voltage.
- **D. f_DATA min 25 kSPS deviation** → Phase 7 watch item; dummy conversion per tick if needed.
- **E. LFXT absent/flaky** → bounded fault loop + VLO/SMCLK fallback keeps streaming with status flag; never hangs.
