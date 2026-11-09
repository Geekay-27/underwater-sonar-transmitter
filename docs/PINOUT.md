# Pinout and wiring

Two separate boards:

1. **Transmitter** - WeAct Black Pill **STM32F411CEU6**
2. **Scope** - **NUCLEO-F446RE** + 1.8" ST7735 160×128 TFT + rotary encoder

Common ground between all boards is mandatory.

## 1. Transmitter (Black Pill STM32F411CEU6)

### MCP4921 12-bit DAC (SPI1, write-only)

| MCP4921 pin | Connects to | Notes |
|---|---|---|
| CS | PA4 | Driven by **DMA** (writes GPIOA->BSRR), not by SPI hardware NSS |
| SCK | PA5 | SPI1_SCK, 12.5 MHz |
| SDI | PA7 | SPI1_MOSI (no MISO needed) |
| LDAC | PB0 | Firmware holds it **LOW**: output updates on CS rising edge. (Alternatively tie it to GND.) |
| VDD | 3.3 V | Black Pill regulator |
| VREFA | 3.3 V | DAC output range = 0 – 3.3 V |
| AVSS | GND | |
| VOUTA | → R → LM358 pin 3 (IN1+) | Coupling resistor into the filter |

### LM358 dual op-amp - 2-pole Sallen-Key low-pass

The LM358 DIP-8 has the same pin numbering as the TL072 in the original table.

| LM358 pin | Connects to | Notes |
|---|---|---|
| 1 (OUT1) | Stage-2 filter network (to pin 5) | First Sallen-Key pole |
| 2 (IN1−) | Feedback network | |
| 3 (IN1+) | MCP4921 VOUTA | Signal input |
| 4 (V−) | GND | Single supply |
| 5 (IN2+) | Stage-1 output via filter network | Second pole |
| 6 (IN2−) | Feedback network | |
| 7 (OUT2) | → coupling cap → TPA3116D2 input | Final filtered output. **Probe here** |
| 8 (V+) | 5 V rail (LM2596) | Op-amp supply |

### Potentiometers (10 kΩ) - simulated environment sensors

| Pot (parameter) | Wiper → STM32 pin | Outer ends |
|---|---|---|
| Bandwidth / centre frequency | PA1 (ADC1_IN1) | 3.3 V and GND |
| Pulse duration | PA2 (ADC1_IN2) | 3.3 V and GND |
| Amplitude / signal power | PA3 (ADC1_IN3) | 3.3 V and GND |

Optional: 100 nF from each wiper to GND to reduce ADC noise.

### On-board items used

| Item | Pin |
|---|---|
| KEY button (mode / window) | PA0 |
| LED (active low) | PC13 |

### Internal peripheral usage

| Resource | Use |
|---|---|
| TIM1 | Sample clock, 250 kHz; update, CC1 and CC4 events raise DMA requests |
| DMA2 Stream 5, ch 6 (TIM1_UP) | CS low |
| DMA2 Stream 1, ch 6 (TIM1_CH1) | Sample → SPI1->DR |
| DMA2 Stream 4, ch 6 (TIM1_CH4) | CS high |
| DMA2 Stream 0, ch 0 | ADC1 → memory (pot readings) |
| SPI1 | 16-bit master, TX-only |
| ADC1 | Channels 1, 2, 3, scan mode |

### Power

| Rail | Source | Feeds |
|---|---|---|
| 5 V | LM2596 buck | LM358 V+, TPA3116D2 logic (check its rating) |
| 3.3 V | Black Pill regulator | DAC, pots |

## 2. Scope (NUCLEO-F446RE)

Arduino-header labels, with the matching MCU pins on the Nucleo-64 (verify against your board's user manual if anything does not respond).

| Function | Nucleo header | MCU pin | Connects to |
|---|---|---|---|
| Signal input | A0 | PA0 | LM358 output (0 – 3.3 V, centred ≈ 1.65 V) |
| TFT CS | D10 | PB6 | TFT CS |
| TFT DC | D9 | PC7 | TFT DC / A0 |
| TFT RST | D8 | PA9 | TFT RESET |
| TFT SCK | D13 | PA5 | TFT SCK / SCL |
| TFT SDA | D11 | PA7 | TFT SDA / MOSI |
| TFT VCC / GND | 3V3 / GND | | TFT power (and LED backlight pin) |
| Encoder A | D5 | PB4 | Encoder CLK |
| Encoder B | D4 | PB5 | Encoder DT |
| Encoder switch | D3 | PB3 | Encoder SW |
| Encoder common | GND | | Encoder GND (the sketch uses internal pull-ups) |

Scope internals: ADC1 channel 0, TIM2 TRGO trigger at 1.5 MS/s, DMA2 Stream 0.

## 3. Cross-board connection

```
LM358 OUT2 (pin 7) ──►(optional 10k series)──► NUCLEO A0 (PA0)
Transmitter GND  ─────────────────────────────► NUCLEO GND
```

The scope is a 0 – 3.3 V unipolar input. Do **not** connect the class-D amplifier output to it.
