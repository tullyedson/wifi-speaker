# BOX-3 display and touch references

BOX-3 wiring, reset polarity and the touch-report handling in `firmware/src/display_panel.cpp` follow Espressif's [BOX-3 BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3), [GT911 driver](https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch/esp_lcd_touch_gt911) and [TT21100 driver](https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch/esp_lcd_touch_tt21100).

SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD

SPDX-License-Identifier: Apache-2.0

The adaptation uses Arduino Wire with bounded report buffers, one contact, and the front capacitive button. It shares the existing Adafruit GFX drawing code between the round and rectangular displays. The full [Apache 2.0 license](es8311/LICENSE) is included. Display commands are supplied by the pinned [Adafruit ILI9341 library](https://github.com/adafruit/Adafruit_ILI9341), version 1.6.2; the [upstream attribution notices](adafruit-ili9341-1.6.2/NOTICE.md) include its README and source-header notices.
