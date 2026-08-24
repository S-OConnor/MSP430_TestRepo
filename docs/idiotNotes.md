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
bench source: the MSP430's eUSCI_B0 SPI is what drives the CLOCK pin, at 0.5MHz, in gated bursts of 24 clocks 
each - one per register access and one per conversion-result frame, idle low in between (allowed by datasheet 
6.3.1.4). Nothing on either 
board is clocked from outside. 0.5MHz is the slowest the ADC accepts in half-clock mode, picked for wiring 
margin - one bit is 2us, and a whole tick's traffic is still only ~135us.

Do not confuse this with the MSP430's own timebase. Two different clocks:

| Clock | What it is | Where it comes from |
| --- | --- | --- |
| ADC CLOCK (0.5MHz) | conversion + serial clock for the ADC | MSP430 SPI, SMCLK 8MHz / 16, P2.2 -> EVM J5 pin 7 |
| tick timer (100Hz) | when a conversion happens | Timer_A0 on SMCLK/8 = 1MHz, period 10000 |

Both come from the MSP430's internal DCO. **No crystal is used at all**: LFXT and HFXT are held off, PJ.4/PJ.5 
stay plain GPIO, and the LaunchPad's onboard 32.768kHz crystal Y4 just sits there unused. That costs ~2% of 
timebase accuracy, which nothing here needs - the tick only has to space the ADC bursts evenly, and the scope 
supplies the timebase for the measurement itself. In exchange there is no crystal start-up window at boot (a 
watch crystal takes hundreds of ms to start, so clock_init() used to spend up to ~1s waiting for it) and no 
crystal-failure mode to handle.

## Conversion:

The analog inputs are held with the CONVST rising edge (conversion start) signal. The setup time of CONVST 
referred to the next CLOCK rising edge (system clock) is 12ns (minimum). The conversion automatically starts 
with the rising CLOCK edge. Do not issue a rising CONVST edge during a conversion (that is, when BUSY is 
high)


## Read Data:
Special read (SR) is NOT used - this is plain Mode II, datasheet 6.5.2.2. One read access = one RD pulse + 
24 clocks = ONE 20-bit frame on SDOA, so the two results of a conversion need TWO read accesses: converter A's 
frame, then converter B's. SDOA is the only data output regardless: M1 is pulled high on the EVM, and the pin 
table above says SDOB is "active only if M1 is low".

Each frame carries an A/B indicator bit (CID=0), and the firmware checks it against the frame it expected 
rather than trusting the order - a swapped or repeated frame is counted in g_err_frame instead of quietly 
swapping the two channels.

RD and CONVST are driven as SEPARATE GPIOs here (RD = P4.2, CONVST = P2.6) so the readout is issued 
explicitly after BUSY drops. Shorting them together is the datasheet's four-wire mode (8.2) and stays in reserve 
as the last rung of the fix-it ladder if frames come back misaligned. The RD signal is triggered by the device on 
the falling CLOCK edge.

