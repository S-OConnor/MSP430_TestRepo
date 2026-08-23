# Design Choices:
We are using Pseudo-Differential 4:1 t to enable access to all four are measured relative to a shared common-mode/reference pin.

# ADC Pins Table

| Signal | Pin | Dir | Description |
| --- | --- | --- | --- |
| BUSY | 23 | DO | Converter busy indicator. BUSY goes high when the inputs are in hold mode and returns to low after the conversion is complete. |
| CLOCK | 22 | DI | External clock input. The range is 0.5MHz to 20MHz in half-clock mode, or 1MHz to 40MHz in full-clock mode. We drive it from the MSP430 SPI (P2.2 = UCB0CLK) at 8MHz |
| CONVST | 19 | DI | Conversion start. The ADC switches from sample into hold mode on the rising edge of CONVST. Thereafter, the conversion starts with the next rising edge of the CLOCK pin |
| ChipSelect | 21 | DI | Chip select. When this pin is low, the SDOx, SDI, and RD pins are active. When this pin is high, the SDOx outputs are tri-stated, and the SDI and RD inputs are ignored. |
| M0 | 17 | DI | Mode pin 0. Selects analog input channel mode |
| M1 | 16 | DI | Mode pin 1. Selects the digital output mode |
| RD | 20 | DI | Read data. Synchronization pulse for the SDOx outputs and SDI input. RD only triggers when CS is low. |
| SDI | 18 | DI | Serial data input. This pin sets up the internal registers. The data on SDI are ignored when CS is high. |
| SDOA | 25 | DO | Serial data output for converter A. This pin is in tri-state when CS is high. |
| SDOB | 24 | DO | Serial data output for converter B. Active only if M1 is low. This pin is in tri-state when CS is high. |


# Additional Notes on ADC
## Clock:
The ADC168M102R-SEP uses an external clock with an allowable frequency range that depends on the mode 
being used. By default (after power-up), the ADC operates in half-clock mode that supports a clock ranging from 
0.5MHz to 20MHz. 

"External" here means external *to the ADC chip* - the part has no oscillator of its own. It does NOT mean a 
bench source: the MSP430's eUSCI_B0 SPI is what drives the CLOCK pin, at 8MHz, in gated bursts (24 clocks for 
a register access, 40 for a readout, idle low in between - allowed by datasheet 6.3.1.4). Nothing on either 
board is clocked from outside.

Do not confuse this with the MSP430's own timebase. Two different clocks:

| Clock | What it is | Where it comes from |
| --- | --- | --- |
| ADC CLOCK (8MHz) | conversion + serial clock for the ADC | MSP430 SPI, P2.2 -> EVM J5 pin 7 |
| ACLK (32.768kHz) | source of the 100Hz sample tick | LaunchPad's onboard crystal Y4, on PJ.4/PJ.5 (LFXT crystal mode) |

Y4 is already fitted on the LaunchPad and neither of its pins reaches a header, so the timebase needs no wiring 
at all. A watch crystal is slow to start (hundreds of ms), which is why clock_init() gives it a ~1s window before 
giving up and falling back to a DCO tick with the ST_NO_LFXT flag set.

## Conversion:

The analog inputs are held with the CONVST rising edge (conversion start) signal. The setup time of CONVST 
referred to the next CLOCK rising edge (system clock) is 12ns (minimum). The conversion automatically starts 
with the rising CLOCK edge. Do not issue a rising CONVST edge during a conversion (that is, when BUSY is 
high)


## Read Data:
RD and CONVST are driven as SEPARATE GPIOs here (RD = P4.2, CONVST = P2.6) so the readout is issued 
explicitly after BUSY drops. Shorting them together is the datasheet's four-wire mode (8.2) and stays in reserve 
as the last rung of the fix-it ladder if frames come back misaligned. The RD signal is triggered by the device on 
the falling CLOCK edge.

