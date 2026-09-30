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

That is a statement about what may be concluded, not about what may be installed.
Fitting both closet units with parts from one batch is right for the measurement
they actually serve: ΔT between intake and exhaust is a difference, so a bias
shared by the pair cancels out of it, and matched age and lot keep the two channels
comparable. The mistake would be taking their later agreement as evidence of
accuracy. A part removed during such a swap is worth keeping for exactly that
reason — the one carrying a characterisation from before an accident is a witness,
even though it is not a reference.

Settling it needs a humidity that is known rather than measured. A saturated
salt solution provides one: undissolved salt keeps the solution at its
saturation concentration, which fixes the vapour pressure above it, and for
sodium chloride that is 75.3 %RH — near enough constant from 20 °C to 30 °C, and
sitting between the two readings in dispute (ASTM E104).

The reference is only as good as the temperature uniformity around it, and that
turns out to be the hard part rather than the chemistry. Since the reading scales
as `es(T_solution)/es(T_sensor)`, a sensor warmer than the solution reads low at
about 4.3 points per °C near 25 °C:

| Sensor above solution | Reads | Error |
| --- | --- | --- |
| 0.2 °C | 74.4 % | -0.9 pt |
| 0.5 °C | 73.1 % | -2.2 pt |
| 1.0 °C | 71.0 % | -4.3 pt |
| 2.0 °C | 66.9 % | -8.4 pt |

Two degrees manufactures 8.4 points, which is larger than the 7.5 the test exists
to settle. The same arithmetic runs the other way through a cold spot: any surface
colder than the solution condenses water, and then the chamber's vapour pressure
is set by that surface rather than by the salt.

How much uniformity is needed comes from the decision, not from the instrument.
The reading in dispute feeds one threshold — 70 %RH on the closet intake, against
a 58 % baseline — so what has to be resolved is which of 67 % and 74 % is right, a
7.5 point question. At ±2 pt the answer is unambiguous, since 73-77 % counts as
agreement with the reference while 67 % is eight points outside it, and ±2 pt
allows 0.45 °C. That is twice the tolerance a ±1 pt answer would need, and nothing
here requires ±1 pt.

Four things make or break it:

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
- **Engineer the gradient away rather than trying to detect it.** A rigid jar
  standing in a bucket of room-temperature water, the whole bucket wrapped, holds
  the chamber isothermal by thermal mass and conduction. A thin bag in open air
  does the opposite: no thermal mass, every draught arriving separately at
  different parts of it, and the lead penetration doubling as a heat path from the
  powered board outside. Seal the leads through a hole rather than clamping them
  in a closure, and coil a length of them inside the insulation so they reach
  ambient before they reach the sensor. A slow leak matters far less than a
  gradient, since the salt keeps regenerating the humidity.

Both parts can go in one container and be read against 75.3 % together.

Two acceptance checks are worth more than trusting the setup, but only the ones
these parts can actually perform. **Drift is measurable**: it is one sensor
against itself over time, so its calibration offset cancels and the 0.1 °C
resolution is what counts — require under 0.1 °C per hour, and a humidity reading
steady for two hours. **Agreement between two sensors is not measurable at this
scale**: with ±0.5 °C on the AM2320 and ±0.5 to 1.0 °C on the BME280, two parts at
one temperature can differ by more than the 0.45 °C being looked for, so "the two
thermometers agree" cannot certify uniformity. What can be used instead is the
*change* in their difference from a value measured beforehand in well-mixed air,
since the offsets are stable — co-located, this pair sat at +0.20 °C all
afternoon. Better still is the water bath, which removes the need to detect
anything.

One ordering note. The BME280 has to be running in forced mode before it goes in,
not after. Its library default keeps the die at a 99.6 % duty cycle, and inside a
small sealed volume with no convection that heat has nowhere to go — a sensor
warmer than the solution reads low at 4.3 points per °C, which is the very error
under test. Left as it is, the instrument writes its own answer.

### Why not a wet and dry bulb

A psychrometer is the other physical reference, and it is the worse one here for a
reason that has nothing to do with chemistry. Humidity from wet and dry bulbs is a
derived quantity: near room temperature it moves about 6 to 7 %RH per °C of
wet-bulb depression, so resolving ±3 %RH means reading both bulbs to about
±0.2 °C, and the wet bulb needs its specified airflow past it as well. A household
static hygrometer has neither the graduations nor the ventilation and lands near
±5 %, the same order as the 7.5 points in dispute — an instrument whose error
matches the effect settles nothing. An aspirated (Assmann) psychrometer would, for
tens of thousands of yen.

The salt solution moves the difficulty somewhere cheaper. Nothing in it has to be
read accurately: while undissolved salt remains, thermodynamics fixes the vapour
pressure, and the only instrument in the experiment is the sensor under test. What
it demands instead is temperature uniformity, bought with a jar, a bucket of water
and a towel rather than with an instrument.

| | What has to be read accurately | Reference accuracy |
| --- | --- | --- |
| Household psychrometer | both bulbs to ±0.2 °C, plus airflow | ~±5 % |
| Aspirated psychrometer | the same, but the instrument provides it | ~±2 % |
| Saturated salt | nothing except the sensor under test | ±0.2 % |

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
- **Get the salt out with water, which is not the same as drying the part.**
  Sodium chloride deliquesces at 75 %RH — the same property that makes the
  reference work — so salt left on the polymer film draws water out of the air
  whenever the room passes that humidity, and the reading never recovers. Drying
  a salted film preserves the fault instead of clearing it. How to get it out of
  a part with no opening to flush is its own problem, below.
- **Dry below 50 °C, over hours.** A heat gun and an ultrasonic cleaner each
  destroy the capacitive film that does the measuring, one thermally and one
  mechanically.

A mute AM2320 has a second and harmless explanation as well: SCL low at the
instant of power-up leaves it in single-bus mode, described under the building
notes below. The two cannot be told apart until the part is clean, dry and
repowered, so nothing should be concluded about the sensor before then — and the
7.5 %RH question the test was meant to settle is still open.

### Getting salt out of a part that cannot be flushed

The AM2320's film sits behind a grille in a closed housing. There is nothing to
wipe and nowhere to direct a jet of water, and by the time anyone notices, the
solution is already inside. Rinsing the outside of it moves none of what matters.

What works instead is dilution: put the whole part in fresh water and change the
water. This is the reasoning behind keeping sea-recovered electronics submerged in
fresh water until they can be cleaned properly rather than drying them on the way
— salt crystallising somewhere unreachable is a worse state than salt still in
solution, and chloride corrosion continues either way.

The arithmetic says how few changes are needed. The part carries perhaps 0.1 to
0.3 mL of saturated brine, at roughly 190 g/L of chloride. Diluted into a litre
that is about 0.06 g/L, already within a few times the 0.02 g/L that tap water
starts at, and the second change puts the interior at the background.

| | Chloride |
| --- | --- |
| Saturated brine | ~190 g/L |
| Tap water | ~0.02 g/L |

So **tap water is sufficient** and distilled water is a refinement rather than a
requirement — worth using for a last rinse if it is to hand, never worth waiting
for. What tap water leaves behind is calcium and magnesium, and those do not
deliquesce; deliquescence is the entire failure mode being cleared.

**Alcohol cannot stand in for the water.** Sodium chloride dissolves 36 g/100 mL
in water and about 0.03 in isopropanol, a thousandfold less, so it cannot do the
one job that needs doing. It has a real use as a water-displacing final rinse on
an ordinary board, but a humidity sensor is the one part where the polymer
absorbs solvent and drifts afterwards, and the only gain on offer is drying
faster.

**Stirring the bath is close to free but is not the variable.** Agitation steepens
the gradient at the mouth of the vent, which helps a little; the rate-limiting
step is diffusion inside the housing, which no amount of stirring reaches. Thirty
minutes per change is the useful setting. Once the chloride is gone, leaving the
part soaking overnight costs nothing and shortens the whole job, because drying
cannot be hurried and a part left half-dry is the single state to avoid.

Drying then wants 30 to 50 °C and the grille in open air rather than face-down on
the warm surface. Gravity takes the first part of the water when the part is
shaken pins-down; the rest has to evaporate and leave through that same opening,
so covering it closes the exit. Desiccant alongside helps and is not necessary.
No power for 24 hours.

### What the timestamps settled, and what they could not

The monitoring turned out to record the repair as well as the fault. Transitions
of `up` for the unit are hard timestamps for the power being cut and restored,
which is how this table was reconstructed after the fact:

| 2026-09-27 | |
| --- | --- |
| 12:30:30 | last plausible reading, 23.2 °C |
| 12:30:45 | `up` 1 → 0; unit unplugged and taken to the bench |
| 16:53:00 | `up` 0 → 1 with temperature 0; **no read has succeeded since** |
| ~21:34 | the sensor is reported to have just gone into the solution |
| 21:37:15 | `up` 1 → 0; power cut |

One caution on reading that record: a 15 s gap in `up` at 20:11 is a restart of
json_exporter, not a fault. A record needs its own artefacts labelled, or the
next person reads a hole as evidence.

The number that decides whether the part survives is how long it was wet **and
powered**, and the record bounds it between about two minutes and four and
three-quarter hours without narrowing it further. Nothing in the data can: once
the published value is zero, getting the sensor wet changes nothing about it, so
a part that was dry and in single-bus mode looks exactly like a part that was
drowned and silent.

The bound is that wide because the report was relative — "just now" — while the
failure had already been visible for hours, and the absolute times had to be
recovered afterwards from `up` transitions and commit timestamps. Reports of
physical work are worth stamping when they arrive.

### The verdict: the film, not the bus

Powered up after a day of drying, the part answered — so the bus was never the
problem, and the silence had been single-bus mode or something equally harmless.
It came up sitting beside the surviving AM2320, which makes the readings a
same-air comparison against a baseline these two parts set before the accident:
on 2026-09-24, co-located, they agreed to 0.2 %RH and 0.3-0.5 °C.

| | Before, same air | After |
| --- | --- | --- |
| Temperature | 0.3-0.5 °C apart | **+0.20 °C, steady** |
| Humidity | within 0.2 % | **+21.7 points, ×1.315, steady** |

The temperature element is intact and the polymer film reads about 1.3× high.
Those are separate parts inside one package — the film measures humidity, a
separate element measures temperature — so ionic residue in the film leaves the
thermometer alone. The rinse recovered a thermometer. It did not recover a
hygrometer, and whether a longer soak would have leached the film is untested.

Three things in the diagnosis were wrong in ways worth keeping:

- **A proportional humidity error produces a constant computed dew point.** Since
  `e = RH × es(T)`, scaling RH scales the vapour pressure, so a part reading 1.3×
  high in air of steady absolute humidity reports a steady dew point. Dew-point
  constancy was read here as evidence the film was tracking physics correctly. It
  is the signature of the error, not a clearance.
- **Deliquescence and contamination are different mechanisms.** A saturated salt
  solution holds the air above it at 75.3 %RH and cannot exceed it, which was used
  to rule salt out at 90 %. Ionic residue *inside* the polymer is the other
  mechanism: it raises water uptake at any humidity, with no ceiling, and it is
  what the ×1.3 looks like.
- **Residual water could not have supplied it.** Fick's law over the grille puts
  0.13 to 1.06 mL through it in the 24 drying hours, for opening areas of 5 to
  40 mm²; the housing holds at most 0.2 to 0.4 mL. Even full, it empties. A
  reading blamed on trapped water needs a reservoir the part does not have.

One practical note: the first reading after moving a part off a warm surface is a
cooling transient, not its steady state. Ten minutes before the numbers above
settled, the same pair differed by +2.9 °C and ×1.05 — which would have read as a
healthy part.

The unit goes back into service as the exhaust sensor regardless. Every alert that
matters in the closet is built on temperature, and exhaust humidity feeds none of
them, so it returns with its humidity channel marked untrusted. What it can no
longer do is take part in the 7.5 %RH argument that started this, which now needs
the other AM2320.

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

### Reading the reports on a sensor that was not there

The design was exercised for real within the day. With no sensor on the bus the
unit publishes no temperature and no humidity at all, `sensor_ok` reads 0,
`read_errors` climbs every three seconds — and `up` stays 1, which is the whole
point. `TempmeterSensorFailing` fired ten minutes in. `TempmeterStuck` stayed
quiet, having no temperature series to find frozen, so one cause produced one
alert. That is the same shape of failure that went unnoticed for four hours two
days earlier.

The diagnosis it supports is worth recording as a sequence. `last_rv` came back
-11, `AM232X_ERROR_CONNECT`, meaning the address phase never drew an ACK — silence
rather than corruption, which rules out the pull-up class of fault whose symptom is
plausible numbers. `i2c_hung_at_boot` came back false, so SDA was not being held by
a slave reset mid-transfer. A run at 20 kHz changed nothing, which rules out
marginal timing, since a clock two and a half times slower rescues a sensor that is
merely close to the edge. Three candidate causes, eliminated from across the house
in minutes.

The part had been unscrewed. `ERROR_CONNECT` covers "not there" as readily as "not
answering", and three flashes went into characterising a sensor that was absent,
because nothing in the process asked what had changed physically. The same omission
had already put the part on a warm surface in an earlier round of analysis when it
was in fact sitting beside its twin, which is where the numbers that settled the
film came from. A report of a remote reading is only as good as the assumption
about what the hardware is doing, and that assumption is cheap to check by asking.

### A new part on a connector that was not connecting

The replacement AM2320 arrived and went onto the exhaust unit, and the unit
reported exactly what it had reported with no sensor on it at all: no
`temperature` and no `humidity` in the JSON, `sensor_ok` 0, `read_errors`
climbing, `last_rv` −11. A fresh part rules out the part, which leaves the
wiring, and the wiring was a female DuPont crimp whose spring had been splayed
open by re-seating until it sat over the header pin without gripping it. Pinching
the contact closed with a pick fixed it — nothing in the firmware or on the bus
changed.

What said "connection" rather than "component" was in the series already, in the
shape of `last_read_age_s` across three windows of one afternoon:

| Window (JST) | What the series did | State |
| --- | --- | --- |
| 16:47–17:14 | Age tracks uptime to 1727 s, errors accrue steadily, no temperature series exists | Never read once |
| 17:15–17:23 | Age resets, then dips twice inside one boot — 19→79, 24→84, 45→285 — and a temperature series appears and immediately freezes | Reads landing occasionally |
| 17:24 onward | Age 0–1 s, `read_errors` 0, values tracking the room | Fixed |

**Intermittency is the signature of a mechanical contact.** A part that is
absent, latched into single-bus mode, or corroded open is mute consistently. A
contact that is nearly touching is mute most of the time and answers when it
happens to touch, and that middle window is the whole diagnosis. `last_read_age_s`
went in to separate never-read from read-and-stopped; it separates a third state
for free, because an age that falls without the uptime falling can only mean a
read succeeded after one failed. The error counter alone does not show this —
twenty failures a minute looks much the same whether one attempt in thirty
succeeds or none do. (The accrual rate itself differed between the two windows,
13 a minute and then 20, on a fixed three-second interval. A failure slow enough
to make the following attempt return `AM232X_READ_TOO_FAST`, which is deliberately
not counted, would produce that, but it was not measured and the rate is not what
the diagnosis rests on.)

**A restart that changes nothing does not point at the part.** One happened
between the first two windows and the unit came back just as mute. The previous
round used "only a power cycle clears a single-bus latch" as a reason to suspect
the sensor; a splayed crimp survives a power cycle every bit as well.

So `ERROR_CONNECT` now covers three things — not there, not connected, and not
gripping. Both times this unit has gone silent the cause turned out to be
mechanical and was found by hand, after remote reasoning had eliminated the
electrical candidates correctly and still not named it.

### The answer, from two new parts

Both closet units were re-sensored with parts from one new batch, which put the
other AM2320 — the one the 7.5 %RH argument had been left waiting on — back in
play. All three units sat in the same place for the comparison, and because they
settle at slightly different temperatures the quantity to compare is vapour
pressure, `e = RH × es(T)`, not relative humidity. Raw RH differences between
units at different temperatures are mostly a picture of the temperature
difference. Comparing `e` also cancels a warm die to first order: the film and the
thermometer share a package, so a part reading its own elevated temperature
reports the RH at that temperature, and the product comes back to the true vapour
pressure.

Taken with the BME280 verified healthy — 30 consecutive samples, pressure
identical across all of them:

| Unit | T (°C) | RH (%) | e (hPa) | Td (°C) |
| --- | --- | --- | --- | --- |
| BME280, indoor | 27.2 | 59.5 | 21.413 | 18.62 |
| AM2320, intake, new | 25.7 | 64.0 | 21.084 | 18.38 |
| AM2320, exhaust, new | 25.7 | 66.7 | 21.973 | 19.04 |

Against the BME280 that is −1.5 % and +2.6 % in vapour pressure, and the BME280
falls **between** the two AM2320s. On 2026-09-24 the same comparison at one
temperature had both AM2320s 11.4 % above it in `e`. The gap is gone, and the
BME280 is no longer the odd part out.

What that licenses is narrow and worth stating exactly. The old pair read high
*together* — which is the shared-lot bias the earlier section said would be
invisible in their 0.2 % agreement. Changing the lot is what made it visible. It
says nothing about any part being accurate, and the new pair agreeing would say
nothing either.

It does change what the salt test is for. The argument it was going to settle no
longer exists; what remains is an absolute anchor on the error the three parts
have in common, which the ±3 %RH part tolerance floors anyway. And the residual is
not small: the two new parts, at a temperature difference of exactly 0.00 °C, are
3.9 % apart in `e` — about 2.5 points of RH, just outside the ±2 points the 70 %
closet-humidity threshold was calculated to need. Matching the lot delivered the
temperature channel and not the humidity channel.

### Everything that has failed here has been a spring

Four faults have been found in this project, and under the symptoms they are one
fault:

| | The contact | Symptom |
| --- | --- | --- |
| Aug–Sep | No pull-ups at all — the internal 45 kΩ carrying the bus | Plausible lies |
| Sep 30, 17:15 | A DuPont female crimp splayed open | Silence, `-11` |
| Sep 30, 17:32 | A pull-up leg loose in its breadboard clip | Lies, then one frozen constant |

The salt-water accident is the one exception, and it is the only failure in the
list that was not a connection.

**The symptom does not follow the fault; it follows the checksum.** The AM2320
carries a Modbus CRC, so a marginal connection reaches the firmware as a failed
read. The BME280's data registers carry none, so the same marginal connection
reaches the firmware as a number. Two parts on the same kind of broken wire,
reporting opposite things, and the difference is entirely in what the protocol
lets the library verify.

The breadboard fault also came with a reminder about what fixed it. A power cycle
was the action, and the sensor came back healthy — which looks like evidence
against a loose contact, because a reset does not tighten anything. It was not the
reset. The board was handled to do it, and the loose pull-up leg could be wobbled
by a fingertip, which is what actually restored the contact. An intervention that
involves touching the hardware proves nothing about the electrical theory it seems
to confirm.

The conclusion is about where the springs are rather than about enclosures. A case
keeps a build from being knocked; it does not make a breadboard clip grip, and the
clip goes on being a spring inside it. The cheap fix is to take the springs out of
the signal path: the two pull-up resistors soldered directly across the breakout's
own pins, VCC to SDA and VCC to SCL, removes two of them for the price of bending
two leads.

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
- **A female DuPont crimp that has been re-seated a few too many times stops
  gripping.** The spring splays open, the socket rests on the header pin without
  making contact, and on I2C that reads as `AM232X_ERROR_CONNECT` — the same thing
  a missing sensor reports. Squeeze the contact shut with a pick before suspecting
  the part; see [above](#a-new-part-on-a-connector-that-was-not-connecting).
- **Solder the pull-ups to the breakout's own pins.** A resistor leg in a
  breadboard clip is one more spring in the bus, and when it loosens a BME280
  reports numbers rather than errors. Bending two 4.7 kΩ leads across VCC-to-SDA
  and VCC-to-SCL costs nothing and takes them out of the signal path.
- Credentials live in NVS under the `wifi-store` namespace, not in the source.

### Known limitation

The setup form submits over plain HTTP with `GET /save?s=…&p=…`, so the Wi-Fi
password appears in the query string on an open access point. It is a
once-per-network exposure on a link you control, but it is not good practice and
a POST over the captive portal would be better.

## License

[MIT](LICENSE)
