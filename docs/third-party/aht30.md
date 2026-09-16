# AHT30 sensor reference

The AHT30 command, CRC-8 check and temperature/humidity conversion in `firmware/src/board_sensors.cpp` are adapted from [Espressif's AHT30 driver](https://github.com/espressif/esp-bsp/tree/7198face0156b70b2878fe3b45d7028b73bd7d09/components/sensors/aht30), component version 1.0.0~1.

SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD

SPDX-License-Identifier: Apache-2.0

The adaptation uses Arduino Wire on the BOX-3 dock bus, bounded I2C transactions, nonblocking conversion polling and explicit unavailable/stale results. It does not include Espressif's sensor-hub integration. The full [Apache 2.0 license](es8311/LICENSE) is included.
