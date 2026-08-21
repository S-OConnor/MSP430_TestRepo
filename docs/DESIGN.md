# Detailed Design (from first principles)

This document explains the whole system assuming you have never used an ADC
or SPI before. It builds up the concepts first, then walks through exactly
what the firmware does and why. Read [ARCHITECTURE.md](ARCHITECTURE.md)
first if you only want the summary.

Contents:

1. [What an ADC is](#1-what-an-adc-is)
2. [What SPI is](#2-what-spi-is)
3. [Why this ADC is not a "normal" SPI device](#3-why-this-adc-is-not-a-normal-spi-device)
4. [The trick that makes SPI work anyway: the gated clock](#4-the-trick-that-makes-spi-work-anyway-the-gated-clock)
5. [The MSP430 side: clocks, pins, peripherals](#5-the-msp430-side-clocks-pins-peripherals)
6. [Talking to the ADC, step by step](#6-talking-to-the-adc-step-by-step)
7. [Reading two channels: why a pair](#7-reading-two-channels-why-a-pair)
8. [Timing: the 100 Hz tick](#8-timing-the-100-hz-tick)
9. [Getting at the data: the scope](#9-getting-at-the-data-the-scope)
10. [Putting it together: one tick, start to finish](#10-putting-it-together-one-tick-start-to-finish)
11. [Number formats and how to interpret the output](#11-number-formats-and-how-to-interpret-the-output)
12. [What can go wrong and how the firmware reacts](#12-what-can-go-wrong-and-how-the-firmware-reacts)
13. [Glossary](#13-glossary)

---

## 1. What an ADC is

An **analog-to-digital converter** measures a voltage and produces a number.
Ours is 16-bit: the number ranges over 65 536 distinct values, so across a
5 V span each step (one "LSB", least significant bit) is 5 V / 65 536 ≈
76 µV.

Three ideas matter for this design:

**Sample-and-hold.** The input voltage is constantly changing. The ADC
first *tracks* the input, then on a command ("CONVST", conversion start) it
*freezes* a snapshot on an internal capacitor. The conversion then works on
that frozen value, so the reading corresponds to one precise instant.

**Conversion takes time and needs a clock.** This ADC is a *SAR*
(successive approximation) converter — it homes in on the answer one bit
per clock cycle, like a binary search. Sixteen bits plus a little overhead
means ~18 clock cycles per conversion. Crucially, *the ADC has no clock of
its own*: it uses whatever we send on its CLOCK pin, both for converting
and for shifting the result out.

**Reference voltage.** The ADC measures the input *relative to* a reference
voltage (V_REF). Full scale = ±V_REF. Ours has a built-in 2.5 V reference
that we enable in software.

### This particular ADC: two converters, eight channels

The ADC168M102R-SEP contains **two** independent 16-bit converters, "A" and
"B", that snapshot at exactly the same instant. In front of each sits a
4-way switch (multiplexer, "mux"):

```
  CHA0 ─┐
  CHA1 ─┤ mux ──> converter A ──> result A
  CHA2 ─┤
  CHA3 ─┘
                                             (both triggered by CONVST)
  CHB0 ─┐
  CHB1 ─┤ mux ──> converter B ──> result B
  CHB2 ─┤
  CHB3 ─┘
```

We tell it which mux position to use (0, 1, 2 or 3), and one conversion
then delivers **channel pair k = (CHAk, CHBk)**.

This firmware needs only **two** channels, and it picks them from the same
mux position: **CHA1 and CHB1** (pair 1). That choice is what makes the
whole acquisition a single conversion per tick — see
[section 7](#7-reading-two-channels-why-a-pair).

### Pseudo-differential inputs and common mode

Each converter actually measures a *difference* between a positive and a
negative input. The mux in front of it can be wired up two ways, and we
choose the second:

| `PDE` | Configuration | Inputs per converter |
|---|---|---|
| 0 | Fully differential 2:1 (datasheet Table 6-1) | 2 pairs, each measured against its own negative pin |
| **1** | **Pseudo-differential 4:1 (Table 6-2)** | **4 single-ended inputs, all measured against a shared common mode** |

In the **pseudo-differential 4:1** configuration the negative input is a
fixed voltage called the **common mode**, and the four CHxx pins per
converter are the positive inputs — eight in total. The same `C[1:0]` bits
that would pick one of two differential pairs now pick one of four
single-ended inputs:

```
  C[1:0]   ADC+     ADC-
    00     CHx0     CMx / REFIOx
    01     CHx1     CMx / REFIOx     <- what this firmware uses
    10     CHx2     CMx / REFIOx
    11     CHx3     CMx / REFIOx
```

We route the internal 2.5 V reference to be that common mode. Consequence: each channel measures its input relative
to 2.5 V, giving a ±2.5 V range around 2.5 V — i.e. 0 V to 5 V:

| Input voltage | Output code (two's complement) |
|---|---|
| 5.0 V | +32767 (0x7FFF) |
| 2.5 V | 0 (0x0000) |
| 0.0 V | −32768 (0x8000) |

---

## 2. What SPI is

**SPI** (Serial Peripheral Interface) is a simple way for two chips to
exchange bytes over a few wires. One side is the **master** (it generates
the clock — the MSP430 here), the other is the **slave** (the ADC).

Wires:

| Name (generic) | Name on MSP430 | Name on ADC | Direction | Purpose |
|---|---|---|---|---|
| SCLK | UCB0CLK | CLOCK | master → slave | Clock: one pulse per bit |
| MOSI | UCB0SIMO | SDI | master → slave | Data *to* the slave |
| MISO | UCB0SOMI | SDOA | slave → master | Data *from* the slave |
| ~CS | (GPIO P1.4) | ~CS | master → slave | "Chip select": low = you are being talked to |

How a byte moves: the master toggles SCLK eight times. On each pulse, one
bit goes out on MOSI *and* one bit comes back on MISO — SPI is always
full-duplex, even if one direction is carrying junk. Which clock *edge*
(rising or falling) each side uses to change and to read data is fixed by
two settings called **CPOL** (idle level of the clock) and **CPHA**
(which edge samples). Get them wrong and you read every bit one slot late —
garbage. For our ADC:

- The ADC **changes** its output bit on the **rising** edge, so we must
  **read** it on the **falling** edge.
- The ADC **reads** our input bit on the **falling** edge, so we must
  **change** it on the **rising** edge.
- The clock **idles low**.

That combination is CPOL = 0, CPHA = 1. In TI's eUSCI register naming that
is `UCCKPL = 0` and `UCCKPH = 0` (TI's phase bit is the inverse of the
Motorola convention — a classic trap, called out in `spi.c`).

```
 SCLK  ___|‾‾|__|‾‾|__|‾‾|__|‾‾|__ ...
             ^      ^                  MSP430 samples MISO on falling edges
          ^      ^                     ADC drives MISO on rising edges
 MISO  ---< b7 >< b6 >< b5 >< b4 > ...   (MSB first)
```

An 8 MHz clock means each bit takes 125 ns, a byte 1 µs.

---

## 3. Why this ADC is not a "normal" SPI device

A typical SPI sensor has a tidy protocol: pull ~CS low, send a command
byte, read back some bytes, release ~CS. This ADC is different in three
ways, and the whole driver design follows from them:

1. **CLOCK is also the conversion clock.** The SAR needs ~18 clock pulses
   to finish converting. Those pulses come from *our* SCLK. So we must send
   clocks even when we have nothing to say, just to make the conversion
   progress.

2. **Two extra control strobes.** Besides ~CS there are:
   - **CONVST** — a rising edge freezes the sample-and-holds and arms a
     conversion (which starts on the *next* CLOCK rising edge).
   - **RD** — a falling edge tells the ADC "start shifting your result out
     on SDOA now", and it *also* opens a 16-clock window in which the ADC
     listens on SDI for a command word.
   These are ordinary GPIO pins on the MSP430 that we pulse under software
   control.

3. **A status output.** **BUSY** goes high while a conversion is running.
   We must not raise CONVST while it is high.

So the ADC has seven wires to us: CLOCK, SDI, SDOA, ~CS, CONVST, RD, BUSY.

---

## 4. The trick that makes SPI work anyway: the gated clock

Because SCLK is also the conversion clock, one might think we need a
free-running clock signal. We do not. The datasheet (§6.3.1.4) says the
clock may be **held static low between accesses** ("burst mode"). And that
is *exactly* what an SPI master naturally does: the clock only toggles
while a byte is being transferred and sits at its idle level (low, for
CPOL = 0) otherwise.

So the design is:

- **Need N clock pulses?** Transfer N/8 bytes. The bytes' contents may or
  may not matter, but the *pulses* always do.
- **Between bursts** the clock is idle-low, and it is in these quiet gaps
  that we pulse CONVST and RD from GPIO. That guarantees a strobe never
  coincides with a clock edge — which is what the datasheet's "strobe no
  longer than one clock period" rule is really protecting against.

There are two subtle consequences the driver handles explicitly:

- The datasheet says a register write becomes *active* "on the clock
  rising edge *after* the 16-clock write". With a gated clock, that edge
  would not arrive until our next transfer. So every register write sends
  **a third, dummy byte** purely to supply that edge (see `write_word()`).
- Some mode bits take effect "one read access late". After configuring,
  the driver performs **two throw-away conversions** so the streaming loop
  only ever sees settled framing (see the end of `adc168_init()`).

---

## 5. The MSP430 side: clocks, pins, peripherals

### 5.1 The MSP430FR5969 in one paragraph

A 16-bit microcontroller with 64 KB of FRAM (non-volatile memory that is
also writable like RAM), 2 KB SRAM, and a set of on-chip peripherals. The
ones we use: **eUSCI_B0** (configured as SPI), **Timer_A0** (for the 100 Hz
tick), and the **clock system** that generates the CPU and peripheral
clocks. That is deliberately the whole list — there is no serial peripheral
in this firmware.

### 5.2 Clock tree

```
 external 32.768 kHz  ─(LFXIN, bypass mode)──> ACLK  32768 Hz ──> Timer_A0
                                                                   (sample tick)
 internal DCO 16 MHz ──┬──────────────────────> MCLK  16 MHz  ──> CPU
                       └──(÷2)────────────────> SMCLK  8 MHz  ──> SPI clock
```

- **DCO** = digitally-controlled oscillator, the chip's internal RC clock.
  16 MHz for the CPU means FRAM needs one *wait state* (FRAM is only rated
  to 8 MHz), which is why `clocks.c` writes `FRCTL0` before raising the
  clock.
- **SMCLK at 8 MHz** is what the SPI divides down from: ÷1 → 8 MHz SCLK,
  which is the ADC's CLOCK.
- **ACLK from LFXIN in bypass mode.** "Bypass" means "there is no crystal,
  an external logic-level clock is being fed in" — the user's 32.768 kHz
  square wave. If it is missing, an oscillator-fault flag stays latched;
  the firmware retries a bounded number of times and then falls back to a
  DCO-based tick, flagging the condition (`ST_NO_LFXT`) rather than
  hanging.

### 5.3 Pin map

| MSP430 pin | Function | Wire to ADC (EVM J5 pin) |
|---|---|---|
| P2.2 | eUSCI_B0 clock (SCLK) | CLOCK (7) |
| P1.6 | eUSCI_B0 SIMO (MOSI) | SDI (15) |
| P1.7 | eUSCI_B0 SOMI (MISO) | SDOA (1) |
| P1.4 | GPIO output | ~CS (9) — held low |
| P2.6 | GPIO output | CONVST (13) |
| P4.2 | GPIO output | RD (11) |
| P1.5 | GPIO input, pulldown | BUSY (5) |
| PJ.4 | LFXIN | external 32.768 kHz |
| P1.0 / P4.6 | GPIO | LEDs (heartbeat / error) |

The two analog inputs are not MSP430 pins at all — they go straight into
the EVM's op-amp buffers on its own headers:

| ADC channel | EVM header pin | Buffer | On SDOA |
|---|---|---|---|
| CHA1 | J2 pin 5 (even pins GND) | OPA4H014-SEP U2C | frame A |
| CHB1 | J1 pin 5 (even pins GND) | OPA4H014-SEP U1C | frame B |

Those buffers need the ±8 V supplies on J3/J4. The other six channel inputs
are left open.

MSP430 pins are multi-function; two "select" bits per pin decide whether
the pin is plain GPIO or belongs to a peripheral. `clocks.c` sets those
(`PxSEL1`/`PxSEL0`) for the SPI and LFXIN pins. FRAM-family parts
also keep all pins in high-impedance after reset until a lock bit
(`LOCKLPM5`) is cleared — done right after pin configuration.

The ADC's mode pin **M0 is strapped to ground on the EVM header** (J5.17).
Together with M1 (pulled high on the EVM) this puts the ADC in "Mode II":
we choose the channel pair with a command word, and results come out on
SDOA only.

---

## 6. Talking to the ADC, step by step

### 6.1 The command word

Every 16-bit word we send to the ADC (during the 16-clock window after an
RD strobe) is a **CONFIG register write**. Its fields, MSB first:

```
 bit 15 14 | 13 12 | 11 10 | 9  | 8  | 7  | 6   | 5   | 4  | 3 2 1 0
     C[1:0]| R[1:0]| PD    | FE | SR | FC | PDE | CID | CE | A[3:0]
```

The ones we use:

| Field | Meaning | Our value |
|---|---|---|
| C | which channel pair to convert **next** | 0..3, rotates |
| R | 00 = "only update C"; 01 = "rewrite the whole register" | 01 at init, 00 during scanning |
| SR | special read: one RD delivers *both* converters' results | 1 |
| PDE | pseudo-differential (8 single inputs vs common mode) | 1 |
| CID | 1 = omit the indicator bits from frames | 0 (we want them for checking) |
| A | an *action*: 0000 nothing, 0001 "read CONFIG back", 0100 software reset, x010/x101 "the next word goes to reference DAC 1/2", 1100 "the next word goes to REFCM" | varies |

The pattern for the "next word goes to register X" actions is a two-step
write: first the CONFIG word carrying the address, then the value.

### 6.2 The initialization sequence (`adc168_init()`)

```
 1. ~CS low                       enable the interface, stays low forever
 2. write 0x0004                  soft reset (A=0100): everything to defaults
 3. write 0x1041                  R=01, PDE=1, A=0001 -> "send CONFIG back"
 4. RD + read 3 bytes             the readback arrives; check bits 11:4 == 0x04
                                  (PDE=1, all else 0). Wrong -> ST_ADC_NOLINK.
 5. write 0x5140                  R=01, SR=1, PDE=1, CID=0, C=01 (real config;
                                  C = ADC_PAIR, so conversion 1 is pair 1)
 6. write 0x1142 then 0x03FF      REFDAC1 <- enable, 2.5 V
 7. write 0x1145 then 0x03FF      REFDAC2 <- enable, 2.5 V
 8. write 0x114C then 0xFF00      REFCM   <- all channels use REFIO1 as common mode
 9. wait 10 ms                    reference capacitors settle (t_REFON = 8 ms)
10. two throw-away conversions    flush the "one read access late" pipeline
```

Step 4 is the **link check**: if MISO is dead (open wire, ADC unpowered,
wrong strap) we read all-zeros or all-ones and the mode bits will not
match — the firmware then sets `ST_ADC_NOLINK`, lights the error LED before
the first tick, and parks the raw value in `g_cfg` for the debugger. Note
that `g_cfg` is the *link-check* readback (`0x1041`), captured at step 4 —
before the operating word of step 5 is written, so it does not carry the
channel selection.

Steps 6–8 matter because the internal references are **off by default**;
without them the ADC would convert against nothing. Step 8 is what makes the
pseudo-differential 4:1 configuration usable: the `CMxx` bits choose the
*internal* reference over the external CMA/CMB pins, and the `Rxx` bits
choose REFIO1 (the 2.5 V DAC from step 6) over REFIO2. Writing `0xFF00` arms
all eight channels even though only two are read — it costs one word and
keeps `ADC_PAIR` a one-line change.

> **A datasheet trap.** §6.3.2.1 says "In pseudo-differential mode, channel
> selection is performed with the SEQFIFO register." That sentence is about
> *automatic* channel selection, which is `M0 = 1` (Table 6-5). We strap
> `M0 = 0` — manual selection through SDI — so `C[1:0]` steers the mux, per
> Table 6-2. SEQFIFO is never written; its reset value has `SL = 00`, whose
> own description reads "Do not use; use mode I or II instead, where M0 is
> 0". Leaving it alone also satisfies REFCM's "set this register after
> setting the SEQFIFO register" ordering note for free.

### 6.3 One conversion + readout (`adc168_read()`)

```
   CONVST  _|‾|________________________________________________
   CLOCK   ____xxxxxxxxxxxxxxxxxxxxxxxx____xxxxxxxxxx...xxxx____
               ^ 24 conversion clocks       ^ 40 readout clocks
   BUSY    ___|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|______________________________
   RD      _______________________________|‾|__________________
   SDOA    ---------------------------------[frame A ][frame B ]
   SDI     [pair cmd]----------------------------[pair cmd]---
```

1. **Check BUSY is low.** (Datasheet: never raise CONVST during a
   conversion.)
2. **Pulse CONVST** (two GPIO writes, ~190 ns, clock is idle).
   Sample-and-holds freeze; conversion is armed.
3. **Send 3 dummy bytes = 24 clocks.** The conversion needs about 18 of
   them; the extra are margin. (We put the channel command in the first
   byte too — harmless if ignored, correct if latched.)
4. **Wait for BUSY low** with a bounded loop (it drops during the burst;
   the wait is a safety net that becomes an error if it times out).
5. **Pulse RD.** The ADC starts driving frame A on SDOA and opens the
   16-clock command window on SDI.
6. **Transfer 5 bytes = 40 clocks.** MISO returns frame A (CHA1) then
   frame B (CHB1); MOSI carries the channel command in the first two bytes.
   Both channels we want are in this one burst — nothing is discarded.
7. **Reassemble and validate.** The 40 received bits are:

```
   bit  39 38 | 37 ......... 22 | 21 20 | 19 18 | 17 .......... 2 | 1 0
        0  0  |  result A       | 0  0  | 0  1  |  result B       | 0 0
        ^  ^                             ^  ^
        |  +-- converter A indicator (0) |  +-- converter B indicator (1)
        +-- constant leading zero        +-- constant leading zero
```

   The 16-bit results are extracted with shifts and masks; the six fixed
   bits (two per boundary) are compared against their required values.
   If any is wrong, the alignment is off and the sample is rejected
   (`ADC168_ERR_BAD_FRAME`).

Total: ~64 clocks ≈ 8 µs of bus time plus a few µs of overhead — about
20 µs, and that is the entire ADC workload of a tick.

---

## 7. Reading two channels: why a pair

We want two channels. The part offers eight, arranged as four pairs, and
the hardware always converts a whole pair at once. So there are two ways to
pick two channels:

| Choice | Conversions per tick | Simultaneous? |
|---|---|---|
| Two channels on the **same** converter (e.g. CHA1 + CHA2) | 2 — one per mux position, with the other converter's result thrown away each time | No: ~20 µs apart |
| Two channels forming a **pair** (CHA1 + CHB1) | 1 | Yes — one CONVST freezes both |

This design takes the second: **CHA1 and CHB1**, i.e. pair 1. Both results
arrive in the single 40-clock readout that `SR=1` gives us, so nothing is
converted and discarded.

The choice also removes a class of bug. The channel-select command is
**pipelined**: the C value sent during *this* readout chooses the mux
position for the *next* conversion. A rotating scan therefore has to stay
one step ahead of itself, and an off-by-one shows up as data in the wrong
columns. Here C is a compile-time constant:

```
  init:     CONFIG word carries C = 1        -> conversion 1 will be pair 1
  tick n:   convert pair 1 (ask for 1)   -> a1, b1
  tick n+1: convert pair 1 (ask for 1)   -> a1, b1
  ...
```

Every access re-asserts the same selection, so the pipeline is
self-correcting: if a command word were ever corrupted on the wire, at
worst one sample comes from the wrong pair and the next access puts it back.

To acquire a different pair, change `ADC_PAIR` in `board.h`. It feeds the C
field of the init CONFIG word, the C field of every per-conversion command,
— nothing else in the firmware refers to the channel numbers.

---

## 8. Timing: the 100 Hz tick

Timer_A0 counts ACLK pulses (32 768 per second) in "up mode": it counts
0 → CCR0, fires an interrupt, and starts over. With CCR0 = 327 the period
is 328 counts:

```
   32768 Hz / 328 = 99.902 Hz     (10.010 ms per tick)
```

Exactly 100 Hz is impossible from 32 768 Hz with an integer divider
(32768 = 2¹⁵ has no factor of 5²), and 328 is the nearest. If a round
number is preferred, 320 gives 102.4 Hz — one constant in `board.h`.

The interrupt handler only sets a flag and wakes the CPU. The main loop
sleeps in **LPM0** between ticks — a low-power mode that stops the CPU but
keeps SMCLK alive. The deeper LPM3 would stop SMCLK, and SMCLK is exactly
what clocks the tick timer in the no-LFXT fallback below — so LPM3 would
leave that configuration asleep forever.

Fallback: if the external oscillator is absent, the timer runs from
SMCLK/8 = 1 MHz with a period of 10 000 → exactly 100 Hz, but only as
accurate as the DCO (~±2 %). `status` bit 0x01 reports this.

---

## 9. Getting at the data: the scope

There is no link to a PC. The ADC's results leave the *ADC*, on SDOA, and
that wire is where you read them — the MSP430 never needs to repeat them.
This is the deliberate simplification of the design: no serial peripheral,
no ring buffer, no interrupt-driven transmit, and no way for a slow host to
perturb the sample timing.

**Where to probe.** SDOA (EVM J5.1) is the data. CONVST (J5.13) is the
trigger: it pulses once per tick, with ~10 ms of quiet either side, so a
rising-edge single-shot capture lands on a whole acquisition every time.
CLOCK (J5.7) gives the analyzer its bit clock, and BUSY (J5.5) shows the
conversion itself. Sample MISO on the CLOCK **falling** edge (§2).

**What one tick looks like.** One 24-clock burst converts, one 40-clock
burst reads out:

```
   CONVST  _|‾|________________________________________________
   CLOCK   ____24 conversion clocks____40 readout clocks_______
   BUSY    ___|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|_______________________________
   RD      _______________________|‾|_________________________
   SDOA    -------------------------[ frame A ][ frame B ]----
```

A logic analyzer with an SPI decoder set to CPOL=0/CPHA=1, MSB first, will
give you the five readout bytes directly; §11 turns them into numbers.

**What the LEDs tell you.** LED1 (red, P1.0) toggles every 50 ticks, so a
steady 1 Hz blink means the loop is running at the right rate — and it
doubles as a free 1 Hz timebase reference on the scope. LED2 (green, P4.6)
latches on if init failed or any frame has ever failed validation.

**What the debugger tells you.** Every value the firmware computes lives in
a `volatile` global, so halting the target with `mspdebug` and dumping them
is the fallback for anything the scope cannot show: `g_sample_a`,
`g_sample_b`, `g_tick`, `g_err_frame`, `g_err_busy`, `g_status`, `g_cfg`.

---

## 10. Putting it together: one tick, start to finish

```
  t = 0        Timer_A0 CCR0 interrupt: g_tick_pending = 1, wake CPU
  t ≈ 2 µs     main loop resumes
  t ≈ 2–22 µs  adc168_read()  (CONVST / 24 clk / BUSY / RD / 40 clk)
                 <-- this is the whole burst the scope sees
  t ≈ 22 µs    publish g_sample_a / g_sample_b / g_tick
  t ≈ 25 µs    LED bookkeeping, back to LPM0 sleep
  t = 10.01 ms next tick
```

CPU is awake well under 1 % of the time, and the bus is idle for 99.8 % of
each tick — which is why a single-shot trigger on CONVST is unambiguous.

---

## 11. Number formats: decoding a readout burst by hand

The 40 readout clocks carry two 20-bit frames back to back:

```
   bit  39 38 | 37 ......... 22 | 21 20 | 19 18 | 17 .......... 2 | 1 0
        0  0  |  result A       | 0  0  | 0  1  |  result B       | 0 0
        ^  ^                             ^  ^
        |  +-- converter A indicator (0) |  +-- converter B indicator (1)
        +-- constant leading zero        +-- constant leading zero
```

As five bytes off an SPI decoder (`b0`..`b4`), that is:

```
   CHA1 code = ((b0 & 0x3F) << 10) | (b1 << 2) | (b2 >> 6)
   CHB1 code = ((b2 & 0x03) << 14) | (b3 << 6) | (b4 >> 2)
```

Both are **signed 16-bit two's complement**, and both were sampled at the
same instant. Voltage ≈ 2.5 V + code × (2.5 V / 32768) = 2.5 V + code ×
76.3 µV, so 0 V ≈ −32768, 2.5 V ≈ 0, 5 V ≈ +32767.

The six constant bits (`b0 & 0xC0 == 0x00`, `b2 & 0x3C == 0x04`,
`b4 & 0x03 == 0x00`) are the sanity check: if they are wrong, the bit
alignment is off and the numbers mean nothing. The firmware checks them on
every frame too, and lights the error LED when one fails.

In the debugger the same two values are already decoded, in `g_sample_a`
and `g_sample_b`. There, both reading exactly −32768 is the firmware's
"this reading was invalid" marker — though a real 0 V input also gives
−32768, so it only means something alongside a rising `g_err_frame` or
`g_err_busy`.

---

## 12. What can go wrong and how the firmware reacts

| Symptom | Likely cause | Firmware behaviour | What to do |
|---|---|---|---|
| Error LED on, `g_status = 0x01` | 32.768 kHz source not reaching LFXIN | Runs at DCO 100 Hz | Check the square wave amplitude (0–3.3 V) and connection to PJ.4; remove crystal Y1 |
| Error LED on, `g_status = 0x02`, `g_cfg = 0x0000`/`0xFFFF` | SDOA/SDI/RD/~CS wiring, ADC unpowered, PHI board still attached | Keeps running; all frames will fail | Check J5 wiring, DVDD/AVDD, M0 strap, PHI removed |
| `g_err_frame` climbing, SDOA looks shifted | Clock phase / strobe timing / long jumper wires at 8 MHz | Bad frames rejected and counted | Set `ADC_SCLK_DIV` to 2 or 4 in `board.h` (risk A in PLAN.md) |
| Frame A and frame B swapped, or a signal in neither | Analog wiring | — | CHA1 is EVM **J2** pin 5, CHB1 is **J1** pin 5 (even pins GND); check `ADC_PAIR` matches the header pins used |
| Both channels read ≈ −32768 or ≈ 0 with inputs applied | References not enabled / not settled | — | Check init ran (error LED off), 2.5 V on EVM REFIO test points, ±8 V op-amp supplies present |
| No bus traffic at all; heartbeat LED dark | Tick timer never fires, or the firmware never got past init | — | Confirm the 32 kHz source, then halt with `mspdebug` and read `g_tick` |

---

## 13. Glossary

- **ACLK / SMCLK / MCLK** — the MSP430's auxiliary, sub-main and main
  clocks (timer, peripheral, CPU).
- **BUSY** — ADC output, high during a conversion.
- **Common mode** — the fixed voltage a pseudo-differential input is
  measured against (2.5 V here).
- **CONVST** — conversion-start strobe; rising edge freezes the sample.
- **CPOL / CPHA** — SPI clock polarity and phase; decide which clock edge
  moves and samples data.
- **DCO** — the MSP430's internal RC oscillator.
- **eUSCI** — the MSP430's serial peripheral block; "A" instances do UART,
  "B" instances do SPI/I²C. Only eUSCI_B0 is used here.
- **FRAM** — ferroelectric RAM; the FR5969's program memory.
- **Gated / burst clock** — a clock that only toggles when needed and idles
  otherwise; what an SPI master emits.
- **LFXIN, bypass** — low-frequency crystal input, configured to accept an
  external logic-level clock instead of a crystal.
- **LPM0** — low-power mode 0: CPU stopped, peripheral clocks running.
- **LSB** — least significant bit; also the voltage of one code step.
- **MISO / MOSI (SOMI / SIMO)** — SPI data lines: master-in-slave-out and
  master-out-slave-in (TI names the same lines slave-out-master-in etc.).
- **Mode II** — this ADC's operating mode with M0=0, M1=1: manual channel
  select, data on SDOA only.
- **Pseudo-differential** — each input measured against a shared fixed
  reference (vs. fully differential: pairs of inputs measured against each
  other).
- **RD** — read strobe; falling edge starts data output and opens the
  command window.
- **REFDAC / REFCM** — ADC registers controlling the internal reference
  DACs and which reference feeds each channel's common mode.
- **SAR** — successive-approximation register, the ADC's conversion
  method (binary search, one bit per clock).
- **SEQFIFO** — the ADC register holding the automatic-mode channel
  sequencer and FIFO. Unused here (manual selection, `M0 = 0`), left at its
  reset default.
- **SR (special read)** — config bit making one RD strobe deliver both
  converters' results.
- **Two's complement** — signed binary encoding; 0x8000 = −32768,
  0x7FFF = +32767.
- **Watchdog (WDT)** — a timer that resets the chip unless periodically
  serviced; running by default on MSP430, so the first line of `main()`
  stops it.
