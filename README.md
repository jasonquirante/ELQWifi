# ELQWifi

ESP32 pocket WiFi firmware using SIM7600G modem and microSD storage.

## Features

- ESP32 WiFi access point (`ELQWifi`)
- SIM7600G LTE data session via PPP
- NAT from WiFi clients to cellular link
- microSD card logging and boot verification
- simple HTTP status page for network and modem state

## Hardware

- ESP32 development board
- SIM7600G modem with 3.7–4.2V stable power
- TM SIM card (Philippines)
- microSD module wired to ESP32 SPI pins

## Build

From the repository root:

```bash
pio run
```

Upload to the board:

```bash
pio run -t upload
```

Open serial monitor:

```bash
pio device monitor -b 115200
```

## Notes

- Default APN is `tm`, with fallback candidates `internet` and `internet.globe.com.ph`.
- SD card chip select is configured on GPIO5 by default.
- WiFi AP is open by default and serves a simple device status page.
- Ensure the SIM7600G has a common ground with the ESP32.
