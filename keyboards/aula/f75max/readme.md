# AULA F75 Max

AULA F75 Max: 75% gasket keyboard with a rotary knob, a 128x128 LCD, per-key RGB, a USB / Bluetooth / 2.4 GHz mode slider and a battery.

* Keyboard Maintainer: [leow149](https://github.com/leow149)
* Hardware: SN32F299-class MCU marked **HFD80CP100** (LQFP-80, a rebranded SONiX SN32F299), UART-linked wireless module (the same protocol as the AJAZZ AK820 Pro's), external SPI flash (stock LCD graphics, not used by this firmware), 128x128 GC9107-compatible LCD.
* USB: `0C45:800A` (as the stock firmware).

## Status

Working on hardware: key matrix (6x15), Windows / Mac / Android layouts, the stock Fn layer, rotary knob (turn / push / Fn+turn), per-key RGB matrix (hardware PWM), Caps Lock LED, Windows-key lock (red Win LED), LCD (connection pill, battery gauge with charge bolt, layout card, link state, live RGB colour/brightness bar, Caps / Win-lock / Fn chips; drawn from primitives, updated per element without flicker), USB / Bluetooth (3 slots, remembered across reboots) / 2.4 GHz via the mode slider, battery percentage and charge state, idle sleep and USB-host-suspend sleep (backlight + RGB off), remote wakeup of the PC, low-battery RGB cut-off at <= 10 %, PC metrics page (CPU / GPU load and temperature, RAM, network; USB only, fed by `tools/f75max_metrics.py`; Fn + knob push switches pages), Caps Word (both Shift keys), mouse keys (USB only), tap dance (two example actions, `TD(0)` and `TD(1)`, active once assigned), VIA (build with `VIA_ENABLE = yes` in a `via` keymap; the VIA definition and keymap are distributed separately because QMK keeps VIA files out of its tree), Fn+Esc to enter the bootloader.

Left disabled on purpose: combos and key overrides (they need personal definitions) and the hardware watchdog (the timer prescaler encoding is unverified on this part and the usual implementation replaces the bootloader-entry function).

Not implemented: the stock boot graphics / GIF upload (they live in an external SPI flash this firmware does not read), on-board macro recording, and whatever the stock firmware's nRF24-style bit-banged radio (SCK P3.5, MOSI P3.6, MISO P1.9, CSN P0.14, CE P0.15) is for (2.4 GHz works through the module without it). Not verified on hardware: the low-battery cut-off.

## Keys

The Fn layer follows the stock firmware (decoded from the stock image and the F75 manual):

| Shortcut | Function |
|---|---|
| Fn+Q / W / E | Android / Windows / Mac layout (Mac swaps Alt and Win; saved) |
| Fn+1 / 2 / 3 | Bluetooth slot 1 / 2 / 3 (tap selects, hold ~1 s pairs) |
| Fn+` (hold) / Fn+R | pair the current wireless mode / select 2.4G |
| Fn+Win | Windows-key lock (Win LED red, "WL" on the LCD) |
| Fn+Tab / Fn+V / Fn+C / Fn+B | lighting colour / next effect / previous effect / on-off |
| Fn+Up / Down, Fn+F6 / F5 | lighting brightness up / down |
| Fn+Left / Right | lighting speed down / up |
| Fn+F1 / F2 | browser home / mail |
| Fn+F7 ... F12 | previous, play/pause, next, mute, volume down, volume up |
| Fn+U / I / O, Fn+Del, Fn+End | Print Screen / Scroll Lock / Pause, Insert, Home |
| Fn + knob push | switch the LCD between the status page and the PC metrics page |
| Fn+Esc | enter the bootloader (this firmware's own addition) |
| Esc + Backspace, held 3 s | enter the bootloader; hard-wired (reads the raw matrix), so it works even if the keymap or VIA has removed Fn+Esc |

## PC metrics

The second LCD page shows CPU and GPU load with temperature, RAM and network speed. A small host program sends the numbers over the keyboard's raw HID interface (USB only; the wireless link carries no data back to the keyboard):

```sh
pip install hidapi psutil            # required
pip install nvidia-ml-py             # NVIDIA GPU
pip install wmi                      # Windows: temperatures / AMD + Intel GPU via LibreHardwareMonitor
python keyboards/aula/f75max/tools/f75max_metrics.py          # --demo sends test values, --once prints one sample
```

Anything the PC cannot provide shows as `--`; if the program stops, the page falls back to `--` after 4 seconds. Verified on Linux (CPU, RAM, network); the GPU and Windows paths are written but not yet tested on real hardware. The packet format is documented at the top of the script, and VIA keeps working alongside it.

## Building

This keyboard needs the hardware-PWM RGB driver change (`drivers/led/sn32f2xx.c`, in this branch) **and** a patch to the ChibiOS-Contrib submodule:

```sh
make git-submodule
cd lib/chibios-contrib
git apply ../../keyboards/aula/f75max/hardware_pwm.diff
cd ../..
qmk compile -kb aula/f75max -km default
```

The patch targets ChibiOS-Contrib commit `5bed8690c4434309e3ab3e14348e70e588eb192f` (the one SonixQMK pins at the time of writing).

## Flashing

Use [SonixFlasherC](https://github.com/SonixQMK/SonixFlasherC):

* From a running QMK build: press **Fn+Esc**; the keyboard re-enumerates as `0C45:7140`.
* If the keymap is broken: hold **Esc + Backspace for 3 seconds** (the LCD shows `BOOTLOADER`).
* First time (still on the stock firmware): short the two pads **under the space bar** (they are covered by foam and insulation; they tie the MCU's BOOT pin, P1.3 = pin 74, to ground) while plugging in USB.

```sh
sonixflasher -v 0c45/7140 -f aula_f75max_default.bin
```

Recovery: the MCU's boot ROM cannot be overwritten, so the pads always work. Restore the stock firmware by flashing the image from Aula's `F75MAX firmware.exe` updater the same way.

## Hardware notes (verified on a real board)

| Function | Pins |
|---|---|
| Matrix columns (15, shared with the LED matrix) | A4 A5 C0 C1 C2 C3 A6 A7 C4 C5 C6 C7 C14 C8 C9 |
| Matrix rows (6) | B14 B15 B19 D19 A19 A18 |
| LED rows (18, R/B/G x 6) | A11 B4 B5 A8 A9 D8 D9 D10 D11 D12 D13 D16 D17 D18 C10 C11 C12 C13 |
| Encoder | A2 / A3 (resolution 2), push = matrix `[0][14]` |
| Mode slider | A10 low = 2.4G, B2 low = Bluetooth, both high = USB |
| Caps Lock LED | B1 (active high) |
| LCD | SPI0: SCK D0, MOSI D2; CS B8, D/C D14, RESET A17, backlight A16 (**active high**) |
| Wireless module | UART2 115200 8N1: TX B7, RX B6 |
| Charger status | B16 (CHRG, low = charging), B17 (STDBY, low = full) |

The panel needs about 150 ms after the RST pulse before its first command (QMK's built-in reset waits only 20 ms), so `display.c` pulses RST itself; without that the screen could stay blank after a soft reboot such as flashing.
The LCD must use **SPI mode 0**; the panel init sequence is the one the stock firmware sends (overridden in `display.c`), with `MADCTL 0xD8`.
The RGB scan (PWM) interrupt must stay more urgent than the SPI interrupt (see `mcuconf.h`), otherwise every LCD redraw makes the whole matrix flash.

## Credits

The LCD fonts (`graphics/opensans*.qff.c`) are bitmap conversions of Open Sans Semibold (Apache License 2.0, Copyright Steve Matteson / Google), generated with `qmk painter-make-font-image` and `qmk painter-convert-font-image`.


Built on the work of the SonixQMK project and in particular Fernando Birra (fpb), whose AJAZZ AK820 Pro port provided the hardware-PWM RGB driver, the ChibiOS-Contrib patch and the wireless module driver. GPL-2.0-or-later, like QMK.
