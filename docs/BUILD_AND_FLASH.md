# Build and flash

## 1. Arduino IDE setup (once)

1. Install the Arduino IDE (2.x).
2. *File → Preferences → Additional boards manager URLs*, add:
   `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`
3. *Tools → Board → Boards Manager*, search **STM32 MCU based boards** (by STMicroelectronics) and install.

## 2. Transmitter - Black Pill F411CE

*Tools* settings:

| Setting | Value |
|---|---|
| Board | Generic STM32F4 series |
| Board part number | BlackPill F411CE |
| Upload method | STM32CubeProgrammer (DFU) *or* STM32CubeProgrammer (Serial/SWD) |
| USB support | CDC (generic 'Serial' supersede U(S)ART) |
| U(S)ART support | Enabled (generic 'Serial') |

Open `firmware/transmitter/SonarTx/SonarTx.ino`.

### Option A - upload directly from the IDE
Put the board in DFU mode (hold **BOOT0**, tap **NRST**, release BOOT0), press Upload.

### Option B - export a hex/bin, then flash with STM32CubeProgrammer
1. *Sketch → Export Compiled Binary*. The `.hex` and `.bin` appear in the sketch's `build` folder.
2. Open **STM32CubeProgrammer**, connect (USB/DFU or ST-LINK), *Erase*, open the `.hex` file, *Download*.
3. Press NRST.

The `.hex` / `.bin` files are git-ignored. To share a ready-to-flash file, attach it to a GitHub **Release**.

## 3. Scope - NUCLEO-F446RE

| Setting | Value |
|---|---|
| Board | Nucleo-64 |
| Board part number | Nucleo F446RE |
| Upload method | Mass Storage (drag-and-drop) or STM32CubeProgrammer (SWD) |

Open `firmware/scope/SonarScope/SonarScope.ino`, Upload. If colours look inverted on the TFT, change `tftCmd(0x20...)` (INVOFF) to `0x21` (INVON).

## 4. Tuning knobs

At the top of `SonarTx.ino`:

| Define | Meaning |
|---|---|
| `FS_HZ` | DAC sample rate |
| `PRI_MS` | Ping repetition interval |
| `BW_MIN_HZ`, `BW_MAX_HZ` | Bandwidth range of POT1 |
| `FC_MIN_HZ`, `FC_MAX_HZ` | Centre-frequency range of POT1 |
| `PULSE_MIN_MS`, `PULSE_MAX_MS` | Pulse duration range of POT2 |
| `AMP_MIN`, `AMP_MAX` | Amplitude range of POT3 |
| `DEFAULT_MODE`, `DEFAULT_WINDOW` | Power-up modulation / window |
| `DEBUG_SERIAL` | 1 = print parameters over USB serial at 115200 baud |

## 5. Troubleshooting

| Symptom | Check |
|---|---|
| No output | LDAC (PB0) low, VDD/VREF = 3.3 V, CS (PA4) toggling |
| DAC idles but never plays | Enable `DEBUG_SERIAL`; `DMA-TIMEOUT` means the DMA/timer chain is not running |
| Staircase on scope | Filter cut-off too high or filter missing |
| Triangular top of sweep | LM358 slew limit; lower POT3 |
| Scope shows `--` | No burst ≥ ~80 mV p-p; check wiring, bias (≈1.65 V) and gain |
