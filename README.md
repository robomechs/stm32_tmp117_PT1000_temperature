# STM32F103C8 BluePill USB Temperature Monitor

This project is a USB-connected temperature measurement setup for the STM32F103C8T6 **BluePill** board.

It combines:

- a **PT1000** analog sensor read by the STM32 ADC,
- a **TMP117** digital sensor connected over I2C,
- direct **USB CDC** communication from the BluePill to the PC,
- and a desktop **Python viewer** for live plotting, software filtering, and CSV logging.

The board connects to the PC **directly over USB** and enumerates as a virtual COM port. No separate USB-UART adapter is required.

## Project Overview

The firmware runs on an STM32F103C8-based BluePill board and streams temperature data over USB CDC.

Current default behavior:

- **PT1000** is sampled through the ADC at **1 kHz**
- the PT1000 ADC path is smoothed in firmware with a fixed-point LPF at about **10 Hz**
- PT1000 temperature is transmitted over USB at **20 Hz**
- **TMP117** is still polled in firmware at **20 Hz**
- TMP117 is **not filtered in firmware**
- TMP117 is **not sent by default**, but the code already supports switching to TMP117-only or both-sensors output

On the PC side, the included viewer can:

- connect to the USB virtual COM port,
- display live values,
- plot incoming data,
- apply a **software LPF** in the GUI,
- show or hide raw and filtered traces independently,
- and log data to CSV.

## Hardware

### Target MCU board

- **STM32F103C8T6 BluePill**

### PT1000 connection

The PT1000 is connected as a simple divider from `3.3 V`:

- top resistor: **PT1000**
- bottom resistor: **1.65 kOhm**
- divider midpoint: `PA0 / ADC1_IN0`

Divider:

`3.3V -> PT1000 -> PA0 -> 1.65k -> GND`

### TMP117 connection

- `PB6` -> `I2C1_SCL`
- `PB7` -> `I2C1_SDA`
- default I2C address: `0x48`

Additional pins:

- `PB8` -> TMP117 `ALERT` input with external pull-up
- `PB9` -> TMP117 `ADD0` output, held low with external pull-down

### USB connection

The firmware uses the STM32 USB CDC device interface and appears on the PC as a virtual COM port.

Project-specific USB notes:

- `PB5` is used for the external USB reconnect/reset wiring
- D+ has an external `1.5 kOhm` pull-up to `3.3 V`
- the firmware generates a reconnect pulse during startup so the BluePill re-enumerates cleanly over USB

## Firmware

Firmware source is in:

- [TMP117_stm32f103c8](TMP117_stm32f103c8)

Main application file:

- [main.c](TMP117_stm32f103c8/Core/Src/main.c)

### Firmware data path

#### PT1000 path

- sampled by `ADC1` on `PA0`
- ADC timing tuned for slower, quieter sampling
- sampled at **1 kHz**
- filtered in firmware with fixed-point arithmetic
- converted to temperature in degrees Celsius
- sent over USB at **20 Hz**

#### TMP117 path

- read over `I2C1`
- polled at **20 Hz**
- kept in firmware for reference / comparison
- **not filtered in firmware**
- **not streamed by default**

### Default USB output

Current default build sends only PT1000:

```text
PT1000=24.875 C
```

With line ending on the wire:

```text
PT1000=24.875 C\r\n
```

### Other firmware output modes

The firmware can also send:

- TMP117 only
- both PT1000 and TMP117 in one line

These modes are selected in [main.c](TMP117_stm32f103c8/Core/Src/main.c) by the `USB_STREAM_MODE` define.

Typical options in the code:

- `USB_STREAM_MODE_PT1000`
- `USB_STREAM_MODE_TMP117`
- `USB_STREAM_MODE_BOTH`

Example combined line:

```text
PT1000=24.875 C TMP117=24.937 C
```

## Desktop Viewer

Viewer file:

- [temperature_viewer.pyw](temperature_viewer.pyw)

The viewer is intended for direct use with the BluePill USB CDC connection.

### Viewer features

- automatic COM port refresh
- connect/disconnect control
- compact COM names such as `COM3`
- live readout for PT1000 and TMP117
- plot view for one sensor or both sensors
- separate **Show raw** and **Show filtered** checkboxes
- software LPF modes:
  - `Off`
  - `EMA`
  - `SMA`
- adjustable visible X-axis window
- `Follow latest` mode
- `Auto Y`
- `Show all`
- `Reset view`
- `Clear plot`
- `Help` button with built-in usage notes
- CSV logging

### Important note about filtering

There are two different filtering layers in this project:

1. **Firmware PT1000 LPF**
   - applied on the MCU to the ADC path
   - fixed-point
   - currently about `10 Hz`

2. **Viewer software LPF**
   - applied inside the PC application
   - tunable in the UI
   - available for display, comparison, and CSV logging

TMP117 currently has **no firmware LPF**. In the viewer, TMP117 can still be filtered in software for visualization.

### Viewer input formats

The viewer accepts the current labeled firmware stream, for example:

```text
PT1000=24.875 C
```

or:

```text
PT1000=24.875 C TMP117=24.937 C
```

It also keeps compatibility with older single-value lines through the `Single line` selector in the UI.

### CSV logging

The viewer can save CSV with these columns:

```text
timestamp_s,pt1000_raw_c,pt1000_filtered_c,tmp117_raw_c,tmp117_filtered_c
```

## Python Requirements

The viewer uses:

- Python 3
- `PyQt6`
- `pyqtgraph`
- `pyserial`

Example install command:

```bash
pip install PyQt6 pyqtgraph pyserial
```

Run the viewer with:

```bash
py temperature_viewer.pyw
```

On Windows, you can also launch the `.pyw` file directly.

## Building the Firmware

This firmware project is based on an STM32CubeIDE-generated STM32F103 project.

Main project file:

- [TMP117_stm32f103c8.ioc](TMP117_stm32f103c8/TMP117_stm32f103c8.ioc)

### Recommended build flow

1. Open the project in **STM32CubeIDE**
2. Open the `.ioc` if needed
3. Let CubeIDE regenerate project files if required
4. Build the project for the **STM32F103C8T6 BluePill**
5. Flash it with your usual ST-Link workflow

### Command-line build

If the local `Debug` build directory already exists, the project can also be built with:

```bash
make -C TMP117_stm32f103c8/Debug all
```

Note:

- `Debug/` is gitignored
- in a fresh clone, it is normal to open the project in CubeIDE first so the build files are present locally

## Repository Layout

- [TMP117_stm32f103c8](TMP117_stm32f103c8) - STM32 firmware project
- [temperature_viewer.pyw](temperature_viewer.pyw) - desktop GUI viewer
- [TMP117_PCB](TMP117_PCB) - KiCad PCB project

Inside the PCB folder:

- KiCad source project is in [tmp117](TMP117_PCB/tmp117)
- [bom](TMP117_PCB/tmp117/bom) contains BOM-related output
- [manuf](TMP117_PCB/tmp117/manuf) contains manufacturing files

## Practical Usage

Typical workflow:

1. Flash the firmware to the **BluePill**
2. Connect the board to the PC via **USB**
3. Wait for the USB CDC virtual COM port to appear
4. Start [temperature_viewer.pyw](temperature_viewer.pyw)
5. Select the COM port
6. Click `Connect`
7. View live temperature, tune software LPF, and log CSV if needed

## Notes

- The repository ignores generated build outputs, Python cache files, KiCad local history, and KiCad backup archives.
- The current default USB stream is PT1000-only, which keeps the USB output simple for the viewer and logging workflow.
- TMP117 acquisition is still active in firmware, so enabling dual-sensor output later is straightforward.
