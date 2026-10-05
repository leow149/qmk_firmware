QUANTUM_PAINTER_ENABLE = yes
QUANTUM_PAINTER_DRIVERS += gc9107_spi
SRC += display.c
SRC += module.c
SRC += graphics/opensans11.qff.c
SRC += graphics/opensans14.qff.c
SRC += graphics/opensans22.qff.c
VPATH += graphics

# Wireless module on UART2 via QMK's custom Bluetooth driver API (same protocol as the AK820 Pro).
BLUETOOTH_ENABLE = yes
BLUETOOTH_DRIVER = custom
SRC += bluetooth/ch582f_ajazz.c
VPATH += bluetooth

# Raw HID: PC metrics for the LCD (VIA builds already have it; VIA's own commands keep working).
RAW_ENABLE = yes

# Optional QMK features
CAPS_WORD_ENABLE = yes
TAP_DANCE_ENABLE = yes
