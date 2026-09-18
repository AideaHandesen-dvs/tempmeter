# tempmeter

The house had no thermometer, so I built one instead of buying one. About ¥1,500
(~$10) of parts.

An ESP32-C3 SuperMini reads temperature, humidity and pressure from a BME280 —
or temperature and humidity from an AM2320 — and shows them on a 4-digit TM1637
display. The same readings are served over Wi-Fi
as a small web page and as JSON. Wi-Fi is configured from a phone through a
captive portal, so no credentials live in the firmware and moving the device to
another network needs no reflash.

[Demo video](https://youtu.be/Tk6F4vo_uOE) (Japanese) — this build, running on
the hardware described below.

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-C3 SuperMini | The board this was built and run on. |
| BME280 breakout | Temperature, humidity, pressure. I2C, address `0x76`. |
| AM2320 (alternative) | Temperature and humidity only. I2C, address `0x5C`, fixed — one per bus. Bare 4-pin parts need 4.7 kΩ pull-ups on SDA and SCL. |
| TM1637 4-digit 7-segment display | |
| 3D-printed enclosure | |

### Wiring

| Signal | GPIO |
| --- | --- |
| I2C SDA — sensor | 8 |
| I2C SCL — sensor | 9 |
| TM1637 CLK | 5 |
| TM1637 DIO | 2 |

## Choosing the sensor

The sensor type and the pin assignments are all in one block at the top of
`src/main.cpp`. The default is the BME280; build for an AM2320 either by
changing `SENSOR_TYPE` there, or by uncommenting the flag in `platformio.ini`:

```ini
build_flags =
    -D SENSOR_TYPE=SENSOR_AM2320
```

The AM2320 has a minimum 2-second interval between readings and has to be woken
before each one, so the firmware polls every 3 seconds and the HTTP handlers
never touch the sensor themselves — they serve the most recent reading.

## Build and flash

With [PlatformIO](https://platformio.org/):

```sh
pio run -t upload
pio device monitor      # 115200 baud
```

The environment is `esp32-c3-devkitm-1`, which is what a SuperMini flashes as.
USB CDC is enabled on boot (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`),
so the serial monitor is the board's own USB port and no separate adapter is
needed. Libraries are declared in `platformio.ini` and fetched on the first
build: Adafruit BME280, Adafruit Unified Sensor, AM232X, and TM1637.

## First run

There are no stored credentials on a fresh board, so it comes up as an access
point:

1. The display shows `AP`.
2. Join the open network `ESP32C3-Setup` from a phone. Any address opens the
   setup page (the device answers all DNS queries and redirects every path);
   `192.168.4.1` works directly.
3. Pick your network from the scanned list — or type the SSID by hand for a
   hidden one — enter the password, and save. The device stores them and
   restarts into the network.

Afterwards it connects on boot, waiting up to 10 seconds before falling back to
the setup access point again.

## Using it

The display alternates every 3 seconds between `t` + temperature and `h` +
humidity, one decimal place each. **The dot next to the leading letter means
Wi-Fi is connected.** Pressure is not shown on the 4 digits — it is on the web
page.

Once on your network, the device's IP serves:

| Path | What it does |
| --- | --- |
| `/` | Dashboard: temperature, humidity, pressure (BME280 only), connection status |
| `/api/data` | JSON, below |
| `/scan` | Rescan for networks |
| `/reset` | Erase the stored credentials and restart into setup mode |

```json
{"temperature":23.4,"humidity":48.2,"pressure":1013.2,"wifi_connected":true,"ip":"192.168.1.42"}
```

On an AM2320 build the `pressure` key is absent rather than null, so a client
can tell "this sensor has no barometer" from "the reading failed" — and can test
for the sensor type with `"pressure" in data`.

The web interface is in Japanese.

## Notes from building it

- **Turn the radio down.** The SuperMini's antenna and regulator do not like
  full transmit power; the firmware sets 8.5 dBm after `softAP()` and drops to
  5 dBm if the chip reports it did not take. Without this the access point is
  unstable.
- **Order matters**: `WiFi.setTxPower()` has to come *after* `softAP()`, not
  before, or it is overwritten.
- The BME280 answers on `0x76` here. Some breakouts strap `0x77` instead.
- **The AM2320's I2C address is fixed at `0x5C`** for every part, so two of them
  cannot share a bus. It also picks its protocol at power-up: if SCL is low at
  that instant it comes up in single-bus mode instead of I2C, and only a power
  cycle changes its mind.
- Credentials live in NVS under the `wifi-store` namespace, not in the source.

### Known limitation

The setup form submits over plain HTTP with `GET /save?s=…&p=…`, so the Wi-Fi
password appears in the query string on an open access point. It is a
once-per-network exposure on a link you control, but it is not good practice and
a POST over the captive portal would be better.

## License

[MIT](LICENSE)
