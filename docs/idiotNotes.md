Design Choices:
We are using Pseudo-Differential 4:1 t to enable access to all four are measured relative to a shared common-mode/reference pin.

ADC Pins Table
BUSY | 23 | DO | Converter busy indicator. BUSY goes high when the inputs are in hold mode and returns to low after the conversion is complete.
CLOCK | 22 | DI | External clock input. The range is 0.5MHz to 20MHz in half-clock mode, or 1MHz to 40MHz in full-clock mode
CONVST | 19 | DI | Conversion start. The ADC switches from sample into hold mode on the rising edge of CONVST. Thereafter, the conversion starts with the next rising edge of the CLOCK pin
ChipSelect | 21 | DI | Chip select. When this pin is low, the SDOx, SDI, and RD pins are active. When this pin is high, the SDOx  outputs are tri-stated, and the SDI and RD inputs are ignored.
M0 | 17 | DI | Mode pin 0. Selects analog input channel mode 
M1 | 16  DI | Mode pin 1. Selects the digital output mode 
RD | 20 | DI | Read data. Synchronization pulse for the SDOx outputs and SDI input. RD only triggers when CS is low.
SDI | 18 | DI | Serial data input. This pin sets up the internal registers. The data on SDI are ignored when CS is high.
SDOA | 25 | DO | Serial data output for converter A. This pin is in tri-state when CS is high.
SDOB | 24 | DO | Serial data output for converter B. Active only if M1 is low. This pin is in tri-state when CS is high.


Additional Converstaion:

The analog inputs are held with the CONVST rising edge (conversion start) signal. The setup time of CONVST 
referred to the next CLOCK rising edge (system clock) is 12ns (minimum). The conversion automatically starts 
with the rising CLOCK edge. Do not issue a rising CONVST edge during a conversion (that is, when BUSY is 
high)


Additional Read Data Notes:
RD (read data) and CONVST are shorted to minimize necessary software and wiring. The RD signal is triggered 
by  the  device  on  the  falling  CLOCK  edge.  