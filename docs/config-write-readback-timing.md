# CONFIG write + read-back — bit-by-bit line states

Signal states at **every CLOCK falling edge** of `adc168_config_cycle()`
([src/adc168m102.c](../src/adc168m102.c)) — the "write CONFIG with A=0001,
then fetch the reply" exchange that `adc168_init()` uses as its link check and
that `main()` repeats once a second in the idle phase.

Two accesses, 24 CLOCKs each, **48 clock cycles total**.

## Why the falling edge

Half-clock mode, CPOL=0 / CPHA=1 (`UCCKPL=0`, `UCCKPH=0` — see
[src/spi.c](../src/spi.c)):

- The ADC **latches SDI on CLOCK falling edges**, so the MOSI column is the bit
  the part actually captures on that edge.
- The ADC **updates SDOA on CLOCK rising edges**, and the master samples it on
  the falling edge, so the MISO column is the bit actually captured that cycle.

So one row = one bit in each direction. CLOCK idles **low** between accesses.

## Legend

| Column | Pin (MSP430 → ADC EVM) | Sense |
|---|---|---|
| **Busy** | P1.5 ← BUSY (pin 5) | H = conversion in progress |
| **Read Data** | P4.2 → RD (pin 11) | H = asserted (active high, rising edge opens the access) |
| **Conv** | P2.6 → CONVST (pin 13) | H = asserted (active high) |
| **MISO** | P1.7 ← SDOA (pin 1) | data from ADC |
| **MOSI** | P1.6 → SDI (pin 15) | data to ADC |
| **Chip select** | P1.4 → ~CS (pin 9) | **L = asserted** (active low) |

`X` = line is driven but the value is not defined by this exchange (see notes).

## Shape of the two accesses

```
  ~CS     ‾‾‾|_______________________|‾‾‾‾‾|_______________________|‾‾‾‾
  RD      ______|‾‾‾|__________________________|‾‾‾|_____________________
  CLOCK   _________|‾|_|‾|_ ... _|‾|_______________|‾|_|‾|_ ... _|‾|_____
                      24 clocks                       24 clocks
                   ACCESS 1: write 0x1041           ACCESS 2: read back
  BUSY    ______________________________________________________________
  CONVST  ______________________________________________________________
```

`RD` rises **before** the first CLOCK rising edge, is still high at that edge
(the edge the ADC samples it on), and is released on the **second** rising edge
— i.e. it is high across falling edge 1 only, and low from falling edge 2
onward. That is `spi_burst_strobe()`'s job; the datasheet shape is SBASAW9
Figure 5-1.

---

## Access 1 — write CONFIG = `0x1041` (`ADC168_W_LINKCHK`)

TX bytes `0x10 0x41 0x00`. CONFIG field layout:
`C[15:14] R[13:12] PD[11:10] FE9 SR8 FC7 PDE6 CID5 CE4 A[3:0]`,
so this word is `C=00, R=01, PD=00, FE=0, SR=0, FC=0, PDE=1, CID=0, CE=0,
A=0001` ("present CONFIG on SDOA at the next read access").

Falling edges 1–16 are the ADC's 16-CLOCK SDI command window; 17–24 are the
extra clocks that deliver the register-activation rising edge inside the same
access.

| Fall # | Busy | Read Data | Conv | MISO | MOSI | Chip select | MOSI bit |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|---|
| 1  | L | **H** | L | X | L | L | C1 = 0 |
| 2  | L | L | L | X | L | L | C0 = 0 |
| 3  | L | L | L | X | L | L | R1 = 0 |
| 4  | L | L | L | X | **H** | L | R0 = 1 |
| 5  | L | L | L | X | L | L | PD1 = 0 |
| 6  | L | L | L | X | L | L | PD0 = 0 |
| 7  | L | L | L | X | L | L | FE = 0 |
| 8  | L | L | L | X | L | L | SR = 0 |
| 9  | L | L | L | X | L | L | FC = 0 |
| 10 | L | L | L | X | **H** | L | PDE = 1 |
| 11 | L | L | L | X | L | L | CID = 0 |
| 12 | L | L | L | X | L | L | CE = 0 |
| 13 | L | L | L | X | L | L | A3 = 0 |
| 14 | L | L | L | X | L | L | A2 = 0 |
| 15 | L | L | L | X | L | L | A1 = 0 |
| 16 | L | L | L | X | **H** | L | A0 = 1 |
| 17 | L | L | L | X | L | L | pad / activation |
| 18 | L | L | L | X | L | L | pad / activation |
| 19 | L | L | L | X | L | L | pad / activation |
| 20 | L | L | L | X | L | L | pad / activation |
| 21 | L | L | L | X | L | L | pad / activation |
| 22 | L | L | L | X | L | L | pad / activation |
| 23 | L | L | L | X | L | L | pad / activation |
| 24 | L | L | L | X | L | L | pad / activation |

**MISO is `X` for the whole of access 1.** RD was asserted, so the ADC *is*
driving SDOA, but what it presents is whatever was last requested — nothing in
this exchange defines it. `write_word()` passes `rx = NULL` and discards it.

### Gap between the accesses

CLOCK low, `~CS` **H**, RD L, CONVST L, BUSY L, MOSI L, MISO tri-stated
(`~CS` high tri-states SDOA within tD6 = 6 ns). No clock edges here.

---

## Access 2 — read the CONFIG frame back

TX bytes `0x00 0x00 0x00`, so **MOSI is L for all 24 edges**. Frame format
(CID = 0, 20 valid bits + 4 padding clocks):

```
 fall 1     fall 2      falls 3..18          falls 19,20   falls 21..24
 [lead 0]  [A/B ind]   [ d15 .. d0 ]          [0]  [0]     [ padding ]
```

MISO values below are the CONFIG register reading back as written, `0x1041`.

| Fall # | Busy | Read Data | Conv | MISO | MOSI | Chip select | MISO bit |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|---|
| 1  | L | **H** | L | L | L | L | leading 0 |
| 2  | L | L | L | X | L | L | A/B indicator |
| 3  | L | L | L | L | L | L | d15 (C1) = 0 |
| 4  | L | L | L | L | L | L | d14 (C0) = 0 |
| 5  | L | L | L | L | L | L | d13 (R1) = 0 |
| 6  | L | L | L | **H** | L | L | d12 (R0) = 1 |
| 7  | L | L | L | L | L | L | d11 (PD1) = 0 ◄ checked |
| 8  | L | L | L | L | L | L | d10 (PD0) = 0 ◄ checked |
| 9  | L | L | L | L | L | L | d9 (FE) = 0 ◄ checked |
| 10 | L | L | L | L | L | L | d8 (SR) = 0 ◄ checked |
| 11 | L | L | L | L | L | L | d7 (FC) = 0 ◄ checked |
| 12 | L | L | L | **H** | L | L | d6 (PDE) = 1 ◄ checked |
| 13 | L | L | L | L | L | L | d5 (CID) = 0 ◄ checked |
| 14 | L | L | L | L | L | L | d4 (CE) = 0 ◄ checked |
| 15 | L | L | L | L | L | L | d3 (A3) = 0 |
| 16 | L | L | L | L | L | L | d2 (A2) = 0 |
| 17 | L | L | L | L | L | L | d1 (A1) = 0 |
| 18 | L | L | L | **H** | L | L | d0 (A0) = 1 |
| 19 | L | L | L | L | L | L | trailing 0 |
| 20 | L | L | L | L | L | L | trailing 0 |
| 21 | L | L | L | X | L | L | padding clock |
| 22 | L | L | L | X | L | L | padding clock |
| 23 | L | L | L | X | L | L | padding clock |
| 24 | L | L | L | X | L | L | padding clock |

After edge 24: `spi_wait_ready()`, then `~CS` returns **H**. Bus fully idle —
CLOCK low, `~CS` high, RD low, CONVST low.

---

## Notes on the `X` entries

1. **Access 1 MISO (all 24 edges).** Driven by the ADC (RD is asserted) but
   carrying no value this exchange defines. Discarded.
2. **Access 2, edge 2 — A/B converter indicator.** `CID = 0` keeps this bit in
   the frame. `adc168_read()` asserts it (0 for converter A's frame, 1 for
   converter B's), but this is a *register* readback, not a conversion result,
   and `adc168_config_ok()` does not look at it. Don't decode it here.
3. **Access 2, edges 21–24 — padding.** The frame is 20 bits; the SPI moves
   whole bytes, so 4 clocks run past the end of it. The part ignores them and
   `frame_data()` masks them off.

## What the firmware actually validates

`adc168_config_ok()` tests **only bits d11..d4 — falling edges 7 through 14 of
access 2** — against `0x04`:

```c
return (((raw >> 4) & 0xFFu) == 0x04u);
```

That is `PD=00, FE=0, SR=0, FC=0, PDE=1, CID=0, CE=0`. Edges 3–6 (C, R) and
15–18 (A) are shown above as they were written, but nothing in the firmware
checks them, so treat those four MISO values as informational rather than as a
pass criterion.

A stuck SDOA fails the test in both directions: all-zeros gives `0x00`,
all-ones gives `0xFF`. So does a one-bit frame slide from a clock-phase or
strobe-timing fault, since PDE=1 at edge 12 is the only set bit in the window.

## Timing

At `ADC_SCLK_DIV = 32` → SCLK = 16 MHz / 32 = **0.5 MHz**, one CLOCK period is
2 µs:

| | |
|---|---|
| One access | 24 clocks = 48 µs |
| RD high time | ~1 clock period, a bit over 2 µs |
| Both accesses | 96 µs of clocking, plus the inter-access gap |
| Idle-phase repeat rate | once per second |

On a scope this is an unmistakable signature: **two RD pulses, 24 clocks each,
with ~1 s of dead bus either side.**
