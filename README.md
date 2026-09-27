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
| BME280 breakout | Temperature, humidity, pressure. I2C, address `0x76`. Needs 4.7 kΩ pull-ups on SDA and SCL unless the breakout carries its own — see [When a sensor lies](#when-a-sensor-lies). |
| AM2320 (alternative) | Temperature and humidity only. I2C, address `0x5C`, fixed — one per bus. Bare 4-pin parts need 4.7 kΩ pull-ups on SDA and SCL. |
| TM1637 4-digit 7-segment display | Optional — see below. |
| 3D-printed enclosure | |

### Wiring

| Signal | GPIO |
| --- | --- |
| I2C SDA — sensor | 8 |
| I2C SCL — sensor | 9 |
| TM1637 CLK — display builds only | 5 |
| TM1637 DIO — display builds only | 2 |

## Choosing the sensor

The sensor type and the pin assignments are all in one block at the top of
`src/main.cpp`. The default there is the BME280; build for an AM2320 either by
changing `SENSOR_TYPE`, or with the flag in `platformio.ini`:

```ini
build_flags =
    -D SENSOR_TYPE=SENSOR_AM2320
```

The AM2320 has a minimum 2-second interval between readings and has to be woken
before each one, so the firmware polls every 3 seconds and the HTTP handlers
never touch the sensor themselves — they serve the most recent reading.

**The AM2320 also needs the I2C clock turned down.** Arduino's default is
100 kHz, which is the part's nominal ceiling — and at that speed it does not
work here at all. Measured on two units, ten reads per step with the Modbus CRC
checked, 10 cm of jumper wire and external 4.7 kΩ pull-ups:

| Clock | Reads OK |
| --- | --- |
| 100 kHz | 0 / 10 |
| 80 kHz and below | 10 / 10 |

Both units gave exactly this, so it is the combination and not a bad part. The
firmware sets 50 kHz for AM2320 builds (`I2C_CLOCK_HZ`), leaving margin below
the cliff. Without that call the sensor answers on `0x5C` and `begin()` reports
success — `isConnected()` only checks for an address ACK — while every actual
read fails with `AM232X_ERROR_CONNECT` (-11) and an I2C timeout. Initialising
cleanly and then never reading is the signature of this, not of a dead sensor.

## Leaving the display off

The TM1637 is optional. Where nobody is going to look at the digits — a closet,
a rack — building with `USE_DISPLAY=0` drops the display entirely: the library
is not included, GPIO 5 and 2 are left alone, and one small heat source and two
wires disappear.

```ini
build_flags =
    -D USE_DISPLAY=0
```

Everything else is unchanged; the readings are still polled every 3 seconds and
served over Wi-Fi. What is lost is the local indication of AP mode and the
temperature/humidity read-out, both of which then exist only on the serial
monitor and the web page.

`platformio.ini` as committed builds for this case — AM2320, no display.

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

1. The display shows `AP` (on a `USE_DISPLAY=0` build, the serial monitor says
   `=== AP Started ===` instead).
2. Join the open network `ESP32C3-Setup` from a phone. Any address opens the
   setup page (the device answers all DNS queries and redirects every path);
   `192.168.4.1` works directly.
3. Pick your network from the scanned list — or type the SSID by hand for a
   hidden one — enter the password, and save. The device stores them and
   restarts into the network.

Afterwards it connects on boot, waiting up to 10 seconds before falling back to
the setup access point again.

## Using it

On a build with the display, it alternates every 3 seconds between `t` +
temperature and `h` + humidity, one decimal place each. **The dot next to the
leading letter means Wi-Fi is connected.** Pressure is not shown on the 4
digits — it is on the web page.

Once on your network, the device's IP serves:

| Path | What it does |
| --- | --- |
| `/` | Dashboard: temperature, humidity, pressure (BME280 only), connection status, signal strength, device ID |
| `/api/data` | JSON, below |
| `/scan` | Rescan for networks |
| `/reset` | Erase the stored credentials and restart into setup mode |

```json
{"id":"A0:B1:C2:D3:E4:F5","temperature":23.4,"humidity":48.2,"pressure":1013.2,"wifi_connected":true,"rssi":-57,"ip":"192.168.1.42"}
```

`id` is the device's Wi-Fi MAC. It is also what the USB serial number is, so the
same string names the device over the air and on the wire — plugged in, it shows
up as `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<id>-if00`.
Use it rather than the IP to tell two devices apart: DHCP will move the address,
the ID does not.

`rssi` is the signal strength in dBm, and is present only while
`wifi_connected` is true — in AP mode there is nothing to measure. Better than
about -70 is comfortable; worse than -80 is where the link starts to fail. It is
on the dashboard too, which is the quicker way to check reception from a phone
while standing in front of wherever the thing is going to live.

On an AM2320 build the `pressure` key is absent rather than null, so a client
can tell "this sensor has no barometer" from "the reading failed" — and can test
for the sensor type with `"pressure" in data`. `rssi` follows the same rule.

The web interface is in Japanese.

## When a sensor lies

A BME280 on marginal I2C pull-ups does not fail. It answers, and the numbers it
returns look like weather.

Nothing in the part protects against this. The data registers carry no checksum,
so a corrupted raw value goes through compensation exactly like a good one and
comes out the other end as a plausible reading. With no external pull-ups the
bus is held up by the ESP32's internal ones — around 45 kΩ, which into ~100 pF
of bus capacitance gives a rise time near 4.5 µs against the 1 µs that 100 kHz
I2C allows. Edges arrive late, bits get sampled wrong, and the library never
knows.

One unit ran for about a month in this state before anyone noticed, reporting a
steady indoor 20.3 °C.

### What it looks like

Sampling `/api/data` once a second for a minute, on one unit, before and after
adding 4.7 kΩ from 3V3 to SDA and to SCL:

| | Distinct readings / 60 | Pressure outside 950–1050 hPa |
| --- | --- | --- |
| Internal pull-ups only | 45 | 79 % |
| External 4.7 kΩ | 14 | 0 % |

The 14 afterwards are the sensor's own noise in the last digit — temperature
wandering over 27.2–27.4 °C, humidity over 59.7–60.7 % — with pressure identical
across all 60 samples. The 45 before are corruption.

Two things identify it, and neither is visible in a single reading:

- **One triple repeats.** Here it was 20.3 °C / 80.6 % / 758.6 hPa, returned 14
  times in that minute and once for 12 hours unbroken. It is what compensation
  produces from registers that read back as their reset value: a constant, so it
  recurs exactly.
- **The bad values fall into clusters,** not a spread. Pressure landed on −63,
  471.6, 558.0, 758.6 and 1258.4 hPa; temperature on 104.2, 180.6 and 188.5 °C.
  Random noise would scatter. Discrete clusters mean specific bit positions are
  flipping, each one displacing the compensated result by a fixed amount.

Restarting does not help, and the distinction matters when diagnosing: a reset
clears a hung bus, and this bus is not hung. It is transmitting, incorrectly.

### The AM2320 does not do this

Its protocol carries a Modbus CRC, which the library checks, so corruption
surfaces as a failed read rather than a wrong number — `updateSensorData()`
keeps the previous value and logs the error code. That is a different failure to
watch for: the readings stop changing instead of going wrong. Something outside
the device has to notice. Under Prometheus,

```promql
stddev_over_time(esp32_temperature_celsius[1h]) == 0
```

catches a stuck sensor on either part, with no firmware change and without
needing a barometer to sanity-check.

## When two sensors disagree

Three units ran side by side for an afternoon: two AM2320 builds and one
BME280 build, all on the same firmware. They had been in two different
locations, reading 3.3–3.6 °C apart and holding that gap steadily for an hour.
Moved into the same air and logged at 15-second intervals, the spread collapsed:

| | Spread across three units |
| --- | --- |
| In their installed positions | 3.3–3.6 °C |
| Same air, settled | 0.3–0.5 °C |

A persistent, repeatable temperature difference between units in different
places is the places, until they have been in the same air and the difference
survives. It is worth doing before concluding anything about the parts: it costs
an afternoon and it settled the question outright.

Humidity did not collapse. In the same air at 28.5 °C:

| Part | Humidity |
| --- | --- |
| BME280 | 66.6–67.1 % |
| AM2320 | 74.1–75.0 % |
| AM2320 | 74.3–75.2 % |

The two AM2320s agree with each other to within 0.2 %. The BME280 sits about
7.5 % below them, and both parts are specified at ±3 %RH, so ±6 % is the worst
case for the pair. One of them is outside specification.

### Ruling out self-heating

The obvious suspect is the BME280 heating itself. Its humidity compensation uses
the die temperature, so a die warmer than the air reports humidity low. The
firmware did not, at the time of this measurement, call `setSampling()`, which
left the Adafruit library default:

```
MODE_NORMAL, SAMPLING_X16 ×3, FILTER_OFF, STANDBY_MS_0_5
```

Three 16× oversampled channels take about 113 ms to convert, with 0.5 ms of
standby between conversions — a duty cycle near 99.6 %, essentially continuous.
Bosch's recommended weather setting is forced mode, 1× oversampling, one sample
a minute. That has since been changed on its own account: the firmware now asks
for forced mode with 1× on all three channels and takes each reading with
`takeForcedMeasurement()`, which brings the duty cycle from 99.6 % to about
0.3 %. The indoor unit has not been reflashed yet, so the figures in this section
are still the ones the old settings produced. The change is not the explanation
for the 7.5 %.

The reason it is not this is that the same die temperature is what the part
reports as temperature, so self-heating has to appear in both readings at once,
in a fixed ratio. Near 28 °C, saturation vapour pressure moves about 6 % per °C:

| | Needed for 7.5 % RH | Observed |
| --- | --- | --- |
| Die above air temperature | ~1.7 °C | +0.2 °C |

Seven times short. Self-heating can account for roughly one percentage point of
the seven and a half. A hypothesis about a sensor that reads two quantities off
one die can usually be checked against the other quantity for free, which is
worth trying before changing any hardware.

### What agreement between two identical parts is worth

Two AM2320s agreeing to 0.2 % is consistency, not correctness — same part, quite
possibly the same production lot, so a shared bias would be invisible. Nothing
measured so far says which of 67 % and 74 % is the true one, and replacing the
odd part out would answer only whether that individual differs from its spare.

Settling it needs a humidity that is known rather than measured. A saturated
salt solution provides one: undissolved salt keeps the solution at its
saturation concentration, which fixes the vapour pressure above it, and for
sodium chloride that is 75.3 %RH — near enough constant from 20 °C to 30 °C, and
sitting between the two readings in dispute (ASTM E104).

Three things make or break it:

- **Undissolved salt has to remain.** Fully dissolved, the concentration is
  whatever the recipe was and the humidity goes with it. With solid salt present
  the excess dissolves or precipitates to hold saturation, which is what makes
  the figure a constant rather than a recipe.
- **Keep the board outside the sealed container**, sensor on leads through the
  seal. A powered board inside a small sealed volume warms one part of it, and a
  sensor warmer than the solution reads low — the same error the test is meant
  to measure.
- **The sensor hangs in the air above the solution and never touches it.** What
  the figure describes is the vapour pressure over the liquid, so the part
  belongs in the headspace, taped to the lid or suspended from it. A sensor in
  the liquid is not reading a known humidity; it is a wet sensor.

Both parts can go in one container and be read against 75.3 % together.

### The first attempt wet the sensor

On 2026-09-27 the exhaust unit's AM2320 went into the solution itself, and it has
read a flat 0.0 °C / 0.0 %RH since. The firmware says how complete that failure
is. A failed read leaves the previous value in place (`updateSensorData()`), so a
sensor that stops answering part way through a day freezes at whatever it last
said; zero is the variable's initial value, which means no read has succeeded
since the unit booted. The bus never came up at all, rather than coming up and
lying — a different failure from the pull-up trouble described earlier, and one
that is distinguishable from it without opening anything, by whether the stuck
value is zero or plausible.

Salt water is an electrolyte, so the damage is not only a matter of getting the
part dry again:

- **Cut the power before anything else.** A wetted board still energised is
  electrolysis across whatever the liquid bridges, and metal migration between
  the electrodes follows within minutes. How much survives is decided by how long
  it stayed powered, not by how long it stayed wet.
- **Rinse with distilled water rather than only drying it.** Sodium chloride
  deliquesces at 75 %RH — the same property that makes the reference work — so
  salt left on the polymer film draws water out of the air whenever the room
  passes that humidity, and the reading never recovers. Drying a salted film
  preserves the fault instead of clearing it.
- **Dry below 50 °C, over hours.** A heat gun and an ultrasonic cleaner each
  destroy the capacitive film that does the measuring, one thermally and one
  mechanically.

A mute AM2320 has a second and harmless explanation as well: SCL low at the
instant of power-up leaves it in single-bus mode, described under the building
notes below. The two cannot be told apart until the part is clean, dry and
repowered, so nothing should be concluded about the sensor before then — and the
7.5 %RH question the test was meant to settle is still open.

### Reporting that nothing was read

Four hours of `0.0 °C` reached the database as though it were weather, which is a
firmware problem rather than a sensor one: `/api/data` reported a number and said
nothing about whether that number had come from a sensor. It now reports both.

- **A measurement that does not exist is not published.** Until a read has
  succeeded, `temperature` and `humidity` are left out of the JSON entirely, the
  same way `pressure` and `rssi` are omitted where they have no meaning. A unit
  that breaks later keeps publishing its last good reading, which is the honest
  thing to do with it, but nothing invents a value that was never measured.
- **Health is published unconditionally**, as `sensor_ok`, `read_errors` and
  `last_read_age_s`, and that is what makes the omission safe. Dropping a key on
  its own would only move the failure from a wrong number to a missing series:
  the exporter turns a missing key into a missing metric and still answers `200`,
  so the scrape succeeds, `up` stays `1`, and the series quietly goes stale — the
  same silence the pull-up fault hid behind for a month. That was measured on the
  exporter actually in use rather than assumed, along with the fact that it
  converts `true` and `false` to `1` and `0`, so the flag needs no numeric twin.
- The web page shows `—` rather than `0.0` before the first successful read, and
  says how many reads have failed. Someone standing in front of the unit should
  not have to know that zero means broken.

`last_read_age_s` is what separates the two ways of being mute: an age that
tracks uptime means nothing has ever been read, while an age shorter than uptime
means it worked and then stopped. That is the distinction this cost a day to
learn.

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
