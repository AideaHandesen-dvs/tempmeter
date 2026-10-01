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
changing `SENSOR_TYPE`, or with the flag — which is what the `closet` environment
in `platformio.ini` supplies, `indoor` supplying the BME280 one:

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

All three units here are built this way, so `USE_DISPLAY=0` sits in the shared
`[common]` section of `platformio.ini` rather than in any one environment.

## Build and flash

With [PlatformIO](https://platformio.org/):

```sh
pio run -e closet -t upload      # AM2320, the two closet units
pio run -e indoor -t upload      # BME280, the indoor unit
pio device monitor               # 115200 baud
```

**There is one environment per kind of unit, and editing `platformio.ini` before
a flash is not part of the procedure.** It used to be: the sensor was selected by
a build flag in the one environment, commented in or out by hand. That is a way
to put the closet's AM2320 firmware on the indoor BME280 in a single command, and
to leave the file un-reverted in the next commit. With environments the only way
to get it wrong is to mistype `-e`, and `-e` is on the screen.

| Environment | Sensor | Unit | id (MAC) | Address |
| --- | --- | --- | --- | --- |
| `closet` | AM2320 | closet intake | `10:00:3B:CC:E8:48` | 192.168.1.195 |
| `closet` | AM2320 | closet exhaust | `10:00:3B:CC:A9:4C` | 192.168.1.112 |
| `indoor` | BME280 | indoor | `08:92:72:91:5D:9C` | 192.168.1.156 |

`default_envs = closet`, because two of the three are that. **`intake` and
`exhaust` are the role labels the two closet units are scraped under, not two
positions** — they stand in the same place, which
[matters to one alert](#the-two-closet-units-are-in-one-place).

**Address the port by MAC, not by number.** `/dev/ttyACM*` is assigned in
enumeration order and moves when anything is re-plugged; with ten ESP32s on one
hub that is a real way to flash the wrong board:

```sh
pio run -e indoor -t upload \
  --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_08:92:72:91:5D:9C-if00
```

The board is `esp32-c3-devkitm-1`, which is what a SuperMini flashes as.
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
0.3 %. The indoor unit was reflashed with it at 20:00 on 2026-09-30 and the step
it produced was measured directly — see [below](#measuring-the-self-heating-instead-of-bounding-it).
The figures in the rest of this section are the ones the old settings produced.
The change is not the explanation for the 7.5 %.

The reason it is not this is that the same die temperature is what the part
reports as temperature, so self-heating has to appear in both readings at once,
in a fixed ratio. Near 28 °C, saturation vapour pressure moves about 6 % per °C:

| Die above air temperature | Needed for 7.5 % RH | Observed |
| --- | --- | --- |
| Inferred from the AM2320 comparison | ~1.7 °C | +0.2 °C |
| Measured across the mode change, 09-30 | ~1.7 °C | **0.33 °C** |

Five times short at the larger of the two, and that one is a measurement. Self-heating can account for roughly one percentage point of
the seven and a half. A hypothesis about a sensor that reads two quantities off
one die can usually be checked against the other quantity for free, which is
worth trying before changing any hardware.

### What agreement between two identical parts is worth

Two AM2320s agreeing to 0.2 % is consistency, not correctness — same part, quite
possibly the same production lot, so a shared bias would be invisible. Nothing
measured so far says which of 67 % and 74 % is the true one, and replacing the
odd part out would answer only whether that individual differs from its spare.

That is a statement about what may be concluded, not about what may be installed.
Fitting both closet units with parts from one batch keeps the two channels
comparable, and matched age and lot are worth having for that alone. What does
**not** justify it is the sentence written here first — that ΔT between intake and
exhaust is a difference, so a bias shared by the pair cancels out of it. The two
closet units stand in the same place, so there is no intake-to-exhaust gradient
for a shared bias to cancel out of; the only thing that difference measures is
[the two parts against each other](#the-two-closet-units-are-in-one-place). The
mistake would be taking their later agreement as evidence of accuracy. A part removed during such a swap is worth keeping for exactly that
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

The reading above is what today's hardware says, and it stands on its own: three
co-located parts, the BME280 verified sample-by-sample first, agreeing inside
±2.6 % in vapour pressure. It stands on one assumption that is examined
[further down](#the-14-c-that-has-not-been-assigned-to-anything) and does not
hold up — that the temperature each part reports is the temperature of the air it
shares with the other two. The residual is not small — the two new parts, at a
temperature difference of exactly 0.00 °C, are 3.9 % apart in `e`, about 2.5 points
of RH, just outside the ±2 points the 70 % closet-humidity threshold was
calculated to need. Matching the lot delivered the temperature channel and not the
humidity channel.

Those figures are one sample each. Repeating the comparison on 09-30 with both
closet units side by side on the bench, 41 samples over twenty minutes, settles
them:

| Closet exhaust minus intake | Mean | sd | Range |
| --- | --- | --- | --- |
| Temperature (°C) | **−0.037** | 0.048 | −0.10 to +0.00 |
| Relative humidity (ratio) | **×1.0410** | 0.0019 | ×1.0359 to ×1.0455 |
| Vapour pressure (%) | **+3.88** | 0.32 | +3.29 to +4.55 |

**The temperature channel is better than the part is specified to be.** Two units
0.04 °C apart is a twelfth of the AM2320's ±0.5 °C. What it does not buy is the
sentence written here first — that this removes the common-mode error in
`ServerClosetAirflowDegraded` and drops the floor on that alert's difference from
±1.0 °C to about ±0.05 °C. Lowering a floor is only worth anything under a signal,
and [that alert has none](#the-two-closet-units-are-in-one-place).

**The humidity channel is not matched, and is not faulty either.** 2.62 points
apart with a standard deviation near 0.1 is a fixed offset rather than noise, and
±3 %RH is what an AM2320 is sold as, so both readings are in specification and
the truth is only pinned to the 3.4-point window where their claims overlap.

That settles what the salt jar is now for. The dispute it was built to decide
[is still open](#the-09-24-comparison-audited) — the 09-24 record puts two old
AM2320s about seven and a half points of RH above the BME280 and says nothing
about which side of that is wrong — but `ServerClosetHumid` still rests on one
channel — the intake unit's humidity — whose absolute value is uncertain by at
least ±1.7 points. What is wanted is one absolute anchor on that one part.
**Standing a second identical part next to it cannot supply that**, which is the
thing these 41 samples demonstrate rather than assert.

What it does **not** support is the obvious next sentence, which was written here
first: that the old pair read high *together* and changing the lot made a
shared-lot bias visible. The 09-24 readings are [sound after
all](#the-09-24-comparison-audited), and they do put both old parts high against
the BME280 — but that convicts the pair against the third part without saying
which side is wrong, which is the same missing anchor one channel over.

### The two closet units are in one place

Everything above compares the two closet units as parts, and that is the only
thing their difference can be compared as: **both units stand in the same spot in
the closet.** That is a fact about where they were installed, not something to be
inferred from what they report, and the 41 bench samples were therefore not a
special condition — they were the normal one, moved onto a table.

The installed record says the same thing over a much longer baseline than the
bench did. Both units in their positions, untouched, 5-minute steps from
09-25 00:00 to 09-26 09:00 (n=396) — the old pair, before the salt accident and
before the re-sensoring:

| Exhaust − intake, installed | Mean | sd | Range |
| --- | --- | --- | --- |
| Temperature (°C) | **+0.205** | 0.058 | +0.10 to +0.40 |
| Relative humidity (points) | **+0.043** | 0.210 | −0.50 to +0.60 |
| Vapour pressure (%) | **+1.273** | 0.423 | +0.24 to +2.66 |

**+0.205 °C installed is the same +0.20 °C this pair held co-located on the bench
all afternoon.** Standing them in their closet positions and standing them side by
side on a table are the same measurement, which is what one place means.

That kills an alert. `ServerClosetAirflowDegraded` fires on exhaust minus intake
above 10 °C for thirty minutes, and its description states the normal value as
2–3 °C. **No 2–3 °C exists anywhere in the record.** Across those 33 hours the
largest ΔT is +0.40 °C and the standard deviation is 0.058 °C, so the threshold
sits about 170 standard deviations away from where the difference actually lives.
The alert cannot report a stopped fan, a blocked intake, or a clogged filter; the
only thing that can reach 10 °C is one of the two parts failing. The 2–3 °C in the
annotation was assumed, never measured, and the rest of that alert's reasoning was
built on top of it.

Detecting airflow from ΔT needs the two parts actually separated — one in the air
entering the closet, one in the air leaving it. Until they are, the closet's
airflow evidence is absolute temperature and nothing else.

The other side of it is that the pair is a continuous null control in one body of
air, 33 hours of it rather than 41 samples, and it has been running the whole
time. Run the same comparison across the lot change:

| Exhaust − intake, same air | ΔRH (points) | Δe (%) |
| --- | --- | --- |
| Old pair, installed, 09-25→09-26 (n=396) | **+0.043** (sd 0.210) | **+1.273** (sd 0.423) |
| New matched-lot pair, bench, 09-30 (n=41) | **+2.62** (sd 0.1) | **+3.88** (sd 0.32) |

**The mismatched pair agreed in humidity to 0.04 points; the matched-lot pair is
2.6 points apart.** Changing the lot tripled the pair's disagreement in vapour
pressure. Neither figure is a gradient — both pairs were in one body of air — so
"matching the lot delivered the temperature channel and not the humidity channel"
is, in the humidity channel, a step backwards from what was there before.

### The 09-24 comparison, audited

This file used to say the 09-24 comparison could not be checked, on the grounds
that "the Prometheus TSDB begins 2026-09-26 19:10". **That start date was wrong.**
It is what `docker inspect prometheus` reports as the container's `StartedAt`, and
the lock file carries the same timestamp — a restart, read as the beginning of a
database. Every tempmeter series actually begins at **2026-09-24 13:27**, and
retention is 90 days, so nothing was lost.

Scanning the retained history for pressures outside 950–1050 hPa — the only
cross-check that exists on a part with no checksum — finds the BME280 corrupting
samples for days before it was ever moved:

| When | Pressure | Temperature | Humidity |
| --- | --- | --- | --- |
| 09-28 17:16 | −64.3 | 27.7 | 65 |
| 09-28 17:29 | −64.3 | 27.7 | **69** |
| 09-28 17:38 | −64.4 | 27.8 | 64.9 |
| 09-29 12:01 | 758.6 | 20.3 | 80.6 |
| 09-29 22:51 | 758.6 | 20.3 | 80.6 |
| 09-30 16:45 | −10363.3 | 24.3 | **29.2** |

Two things fall out of that table. **The 09-23 repair never held** — adding the
resistors fixed the waveform, and their legs sitting in breadboard clips left the
fault free to come back, which it had done by 09-28 at the latest. And **a corrupt
sample can carry a plausible temperature beside a badly wrong humidity**: 29.2 %
in air that was near 60, and 69 % between neighbours reading 65. Nothing in the
temperature or humidity channel marks either one. Only the pressure does, and it
is wrong in the same sample, at the same instant.

**Audited, the 09-24 comparison holds up.** The three units were in one place, and
15:00–15:20 is present at fifteen-second resolution with no gap: all 81 samples
read 1007.10 to 1007.20 hPa. The corruption 09-24 does contain is at 18:30:15
(1255.5 hPa) and 18:40 (−63.9 hPa), three hours after the comparison. The indoor
humidity falls 71.8 → 60.6 across the window in steps of 0.0 to −1.2 points,
tracking the two AM2320s' own shape from seven and a half points above it — the
fault's signature is a frozen constant, as at 09-30 17:44–18:12 where
758.6 / 20.3 / 80.6 repeats for 27 minutes, and there is nothing of that here. The
figure itself is in the database: at 15:05:00 both AM2320s read **+11.64 %** above
the BME280 in `e`, at 15:06:30 **+11.45 %** and **+11.75 %**.

So the 7.5 %RH argument is **not** void. It is a real three-way disagreement: two
old AM2320s agreeing with each other to 0.04 points and both some seven and a half
points of RH above a BME280 that was not corrupting samples at the time. What is
still missing is the same thing that was missing before — a side of it that can be
tied to a fixed point. The BME280 is the one with a demonstrated fault history, but
"not corrupting samples" is a statement about the fault and not about accuracy: a
clean register read does not make a part right, and convicting the old lot needs
the BME280 to have been correct, which is one assumption further than the data
goes. The three parts agree today, which is the useful half. The one part that
could still speak to 09-24 is the old intake AM2320, kept for this reason: put
beside the new pair it says whether that individual reads high, which is a smaller
claim than the lot but a measurable one.

### Nothing was watching the only channel that could tell

Not one alert fired for any of those six samples, or for the sporadic phase on
09-30 that preceded the freeze. `TempmeterStuck` needs an hour of a motionless
series and single bad samples never produce one; `TempmeterSensorFailing` reads a
flag the BME280 build sets from a read that succeeded. Three days of a
progressively failing bus, and the monitoring said nothing.

The barometer is the canary, and it is the only one available on this part. One
sample outside 950–1050 hPa is already proof of a corrupted transfer — no
averaging, no `for:` duration, because the fault is sporadic by nature and waiting
for persistence is waiting for it to get worse. A rule that simple would have
named this on 09-28, two days before it went total, and it is the piece of this
that generalises: a part without a checksum needs one channel whose valid range is
narrow enough to betray the others.

The rule exists now — `TempmeterPressureOutOfRange`, `for: 0m` — but not on the
950–1050 hPa band first written down here. **What the part reports is station
pressure, uncorrected to sea level**, and a strong typhoon making landfall in
Japan reaches into that band on its own: 950 to 970 hPa is an ordinary central
pressure, and altitude takes the station reading lower still. The band is
900–1080, outside anything the weather does and inside nothing this fault
produces. Backtested over the six hours containing the failure, the two bands
catch the same 154 samples: the corrupt values were −64, 758.6, −10363 and
256470 hPa, and the innermost of them is 140 hPa clear of the wider limit.
Widening it cost no detections and removed a class of false alarm.

The converse stays unavailable, and the rule's comment says so. A pressure
inside the band is not evidence that the reading beside it is sound — corrupt
samples can land anywhere. This is a sufficient condition for distrust and not a
necessary one.

It is also not academic. `ServerClosetHumid` fired on the intake unit for nine
hours on 09-28, on the old sensor, at the 70 % threshold whose accuracy is exactly
what was in dispute. Whether that was a real closet or a high-reading part is one
more thing the missing data does not say.

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

### Measuring the self-heating instead of bounding it

The indoor unit was reflashed at 20:00 on 2026-09-30, and a one-second poll
happened to be running across the change, so `MODE_NORMAL` at 16× and forced mode
at 1× were both recorded on the same instrument. The intake AM2320 went through
untouched, which makes it a reference for the room's own drift:

| Indoor minus intake | Before | After | Change |
| --- | --- | --- | --- |
| Temperature (°C) | +1.756 (sd 0.139) | +1.422 (sd 0.054) | **−0.334** |
| Relative humidity (points) | −5.818 (sd 0.269) | −5.382 (sd 0.180) | +0.436 |
| Vapour pressure (%) | +0.742 (sd 0.588) | −0.528 (sd 0.341) | −1.271 |

**The temperature step is clean, and its shape says it is the die and not the
board.** The offset goes from +1.7 to +1.4 within one scrape and then sits there
for the next half hour with no trend at all. A board shedding a third of a degree
has a thermal mass and would show a curve over minutes; a die whose duty cycle
fell from 99.6 % to 0.3 % settles in seconds. The old configuration was running
the part 0.33 °C above the air it was measuring.

That is the number the self-heating argument never had. At 5.85 %/°C near 27 °C
and 58 %RH it comes to about 1.1 points of relative humidity — which is what
[the estimate above](#ruling-out-self-heating) said, now from a measurement of
this part rather than an inference from a different one.

**The humidity side does not close, and it should have.** Under self-heating
alone the reported vapour pressure is invariant: the film reads the RH at the
die's temperature, the part reports that same temperature, and `e = RH × es(T)`
hands back the air's true value whatever the die is doing. Removing 0.334 °C
should therefore have lifted RH by 1.13 points and left `e` where it was. RH rose
0.44 points and `e` moved 1.27 %.

That was first written up as unresolved, with the control blamed: the two units
read 1.4 °C apart, so perhaps they are not in one body of air and 1.27 % of drift
between two spots in a room is unremarkable. **The excuse does not survive, for
two reasons.**

The first is that all three units are sitting in one place on the bench, which is
a fact about where they are and not something to be inferred from what they
report. Reading a temperature difference as evidence of separate air, when that
difference is the instrument disagreement under investigation, is circular.

The second is that there is a null control in the same recording, and it was not
used. The two AM2320s went through the reflash untouched, in the same air, and
their difference says how much a pair of parts wanders across this boundary:

| Across the 20:00 boundary | ΔT (°C) | ΔRH (points) | Δe (%) |
| --- | --- | --- | --- |
| Exhaust − intake, before | +0.019 (sd 0.040) | +2.441 (sd 0.050) | +3.952 (sd 0.275) |
| Exhaust − intake, after | −0.038 (sd 0.049) | +2.605 (sd 0.057) | +3.882 (sd 0.310) |
| **Step** | **−0.057** | **+0.164** | **−0.070** |
| Indoor − intake, step | −0.295 | +0.402 | **−1.073** |
| Indoor − exhaust, step | −0.238 | +0.237 | **−0.967** |

Windows are 19:52–20:00 (n=32) and 20:01–20:30 (n=117); the 19:36–19:52
excursion, where a hand near the indoor unit put it 2.6 °C above the pair and its
`e` up 15 %, is excluded. **Two parts in the same air held `e` to 0.07 % while the
BME280 moved a full percent against either of them.** Drift between spots is not
what this is.

**And the step is not a contradiction either — it measures something.** The
invariance argument assumes the film and the thermometer are at one temperature.
They need not be. Take the two limits, at 5.86 %/°C and 58 %RH:

| If the film's temperature | RH should step | `e` should step |
| --- | --- | --- |
| follows the die exactly | +1.00 point | 0 % |
| stays at the air's value | 0 points | −1.73 % |

The observation sits between them, and both channels put it in the same place.
Against the intake unit, RH gives 0.40 of the way and `e` gives 0.38; against the
exhaust unit, 0.29 and 0.31. **The film followed about a third of the die's
excursion.** Self-heating at the thermometer was 0.30 °C and at the film about
0.11 °C, which is the ordinary result that the thermometer sits closer to the heat
than the film does. No unknown fault in the humidity channel is needed, and the
salt jar is not what this question wanted.

One cost is visible and was expected. Dropping oversampling from 16× to 1× put
the noise up: over ten-minute windows the pressure standard deviation went from
0.000 to 0.048 hPa and the temperature's from 0.018 to 0.048 °C. That is what the
duty cycle was bought with. It also happens to help `TempmeterStuck`, which fires
on a standard deviation of exactly zero.

### The 1.4 °C that has not been assigned to anything

The same recording contains a larger number than any of the above, and it had
been read as a property of the room. After the reflash the BME280 carries no
meaningful self-heating — 0.3 % duty cycle — and it still reads **1.405 °C above
two AM2320s standing in the same place**. Subtracting the measured self-heating
from the 1.4 °C is wrong and was done here once: the 0.295 °C is already gone from
the post-reflash figure, and 1.405 °C is what is left after it.

So the instrument disagreement in temperature is **4.8 times the self-heating that
was measured so carefully**, and unlike the self-heating it is not attributed to
anything.

It does not stay in the temperature channel. `e = RH × es(T)` puts `es` at
5.86 %/°C near 27 °C, so 1.405 °C is **8.2 % of vapour pressure** — twice the
spread the three-unit comparison was declared to agree within:

| Rests on | Claim | At stake |
| --- | --- | --- |
| [Three co-located parts agree within ±2.6 % in `e`](#the-answer-from-two-new-parts) | the BME280 falls between the two AM2320s | 8.2 % |
| `ServerClosetHumid` at 70 % | the intake unit's RH is good to ±2 points | — |

Correct the BME280 downward by 1.405 °C and its `e` drops 8.2 %, out from between
the two AM2320s and below both. Correct the AM2320s upward instead and theirs
rises by as much. **Three parts in one place fix the differences and not the
errors**, and two of the three came out of one batch, so a bias they share is not
observable here at all.

That is the same shape of conclusion the salt jar reached, one channel over: what
is missing is an absolute anchor, and a second identical part cannot be one. For
humidity that anchor is a saturated salt solution. For temperature it is easier
than that — an ice bath is 0.00 °C, holds it while ice remains, and needs crushed
ice and water. Until one of the three is tied to a fixed point, the 1.405 °C is
unsplit and every `e` comparison across the two sensor types carries it.

### Tapping a contact proves one direction only

After the loose pull-up leg was re-seated — pushed into a hole alongside a jumper
pin, so the 0.64 mm pin spreads the clip and the thin leg is pinched against it —
the unit was polled once a second while the board was deliberately provoked:
resistors flicked, legs levered, the breakout pressed and twisted, jumpers
wobbled, the breadboard tapped. 196 samples over 215 seconds with no gap longer
than two, and the pressure took two values the whole time, 1010.3 and 1010.4.

The handling is visible in the log, which is the only reason the provocation can
be shown to have landed inside the recorded window at all: a hand near the part
puts the humidity up, and three excursions appear, the first peaking at 63.7 %
against a quiet 57.6 %. The pressure does not move in any of them.

**The negative is weak, and the reason is that the test's sensitivity is
unknown.** Nothing has ever shown this fault to be tap-inducible. The 09-30
onset — one bad sample at 17:28, total by 17:47 — has no recorded mechanical
provocation behind it, and the one documented case of a finger on this hardware
*restored* the contact rather than breaking it. Mechanical disturbance is
associated with both ends of the fault and the transfer function between them is
not known. What the test rules out is a contact hanging by a thread. It cannot
say the repair holds.

Time can say that, and now there is something to count with. The 09-23 repair
lasted five days, so five days without `TempmeterPressureOutOfRange` firing is
the first interval worth anything — and unlike the three silent days in
September, the counting is automatic.

### Nothing here has an absolute value

The question all of this is in aid of is whether these units need calibrating, and
the wrong answer is attractive enough to be worth writing down. Reason from the
thresholds instead of from the instrument and it runs: `ServerClosetHot` fires at
35 °C and the closet's record peaks at 28.6, so a 1.4 °C disagreement cannot move
that decision, while `ServerClosetHumid` fires at 70 %RH against a reading near 68,
so its 2.6-point disagreement can. Temperature then reads as settled and humidity
as needing work. The arithmetic is right and the question is not the one that was
asked. **A threshold is a consumer; what a consumer does with a number establishes
nothing about the number.** The 1.4 °C does not stop mattering because today's
closet sits far from 35 °C — it stops mattering *at today's closet*, which is not a
fixed quantity. A reading is finished when it means something on its own, and
neither channel here does.

**Temperature has no absolute value.** The indoor BME280 reads 1.4 °C above two
AM2320s in the same place and nothing present says which is right
([above](#the-14-c-that-has-not-been-assigned-to-anything)). The two AM2320s agree
to 0.04 °C, which is consistency and not correctness — one batch, so a bias they
share is invisible in the only comparison available. Whether either is inside the
±0.5 °C the part is sold as cannot be asked without a reference.

Measured again after all three units were reflashed on 09-30, 20:15–22:30 (n=27):

| °C | Mean | sd |
| --- | --- | --- |
| Indoor − intake | **+1.052** | 0.304 |
| Exhaust − intake | **−0.059** | 0.056 |

The pair figure is the bench figure again — 0.04 to 0.06 °C between two parts of
one lot, now measured three ways. The cross-type figure is the one that will not
hold still: at hourly resolution through that evening it runs +1.56, +1.40, +0.93,
+0.75. **1.405 °C is not an offset to subtract; it is where the disagreement
happened to be during the window it was measured in.**

**Humidity has no absolute value either, and there the window is narrower.** The
two closet parts are 2.62 points apart and both inside ±3 %RH, which pins the
truth to the 3.4-point band where their claims overlap and says nothing about
where in that band either part sits. Reading the intake alone — which is what
`ServerClosetHumid` does — inherits all of it.

The references are the ones already described, and neither needs a second
instrument: an ice bath is 0.00 °C while ice remains, a saturated sodium chloride
solution is 75.3 %RH while salt remains undissolved, and in both the part under
test is the only thing being read.

Three things went wrong on the way here and are worth the space:

- **`up` is the board's liveness, not the sensor's identity.** Both closet units
  were re-sensored on 09-30 and `up` stayed 1 throughout — the MAC, the instance
  label and every series continuous. A sensor swapped on a powered board leaves no
  trace in any of them, and `up` was used here as evidence that the part had not
  been changed. The record cannot answer that question; asking can.
- **A metric has a start date.** `esp32_sensor_ok` first exists at 09-29 13:00 for
  exhaust, 09-30 19:30 for intake and 09-30 20:30 for indoor, with
  `esp32_sensor_read_errors_total` and `esp32_sensor_last_read_age_seconds` beside
  it. The 09-27 failure was described here in terms of all three, none of which
  existed yet. Check that a series covers a window before reading that window
  with it.
- **A quantity this file does not define is not an indicator.** One was invented
  mid-session — the difference between the two closet humidities, given a name and
  reasoned with — and it appears nowhere in this project. Unpacked it carried
  nothing the two series side by side did not already carry. A new quantity
  belongs in this file before it belongs in an argument.

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
- **An older build does not report its own MAC**, which leaves it unidentifiable
  among ten ESP32s on one hub. `ping` it once and read `ip neigh show <ip>`: the
  Wi-Fi MAC the ARP table gives back is the same string the USB JTAG serial
  number is built from, so it maps straight onto `/dev/serial/by-id/`. That is how
  the indoor unit was tied to `08:92:72:91:5D:9C` before it was reflashed — and
  the `id` the new firmware then reported confirmed the right board had been
  written.
- Credentials live in NVS under the `wifi-store` namespace, not in the source.

### Known limitation

The setup form submits over plain HTTP with `GET /save?s=…&p=…`, so the Wi-Fi
password appears in the query string on an open access point. It is a
once-per-network exposure on a link you control, but it is not good practice and
a POST over the captive portal would be better.

## License

[MIT](LICENSE)
