# Orange Defect Detection (ESP32 + Laptop)

A simple sorting machine with no Raspberry Pi:

- **ESP32** — all the hardware: load cell, rotation motor relay, conveyor
  relay and 3 servos.
- **Laptop** — the USB/built-in camera plus all the processing
  (`app.py`): decides `DEFECTED` or `NORMAL` using basic color/black-patch
  detection.
- **MQTT** (HiveMQ free public broker `broker.hivemq.com`) is the glue
  between the two — no local broker to set up.

Files in this project:

| File | Runs on | Purpose |
|---|---|---|
| `app.py` | Laptop | MQTT client, camera defect detection, tells ESP32 when to rotate |
| `esp32_code.ino` | ESP32 | WiFi/MQTT, load cell, rotation + conveyor relays, 3 servos |
| `test.py` | Laptop | Sends motor ON/OFF over MQTT to check the rotation relay |
| `README.md` | — | This file |

## How it works

1. Orange is placed on the platform. The ESP32's load cell (HX711) detects
   weight above `WEIGHT_PRESENT_THRESHOLD_G` (default 5 g), waits until
   the reading stops changing (so a half-placed orange isn't weighed),
   and publishes `OBJECT_DETECTED` on MQTT topic `orangesort/detect`.
2. The laptop receives that message and publishes `ON` on
   `orangesort/motor`; the ESP32 turns on the **rotation motor relay**.
   After `ROTATE_SECONDS` (default 3 s, one full rotation) the laptop
   publishes `OFF`. It samples camera frames while it spins. A live window shows the camera
   through several filters the whole time (see "Live video window").
3. Each frame is converted to HSV and checked for a big patch of black
   pixels (rot/mold typically looks like a large dark/black blotch). If
   any sampled frame has a black patch bigger than `BLACK_AREA_THRESHOLD`,
   the orange is marked defected.
4. The laptop stops the motor (`OFF`) and publishes the result (`DEFECTED` or
   `NORMAL`) on MQTT topic `orangesort/result`.
5. The ESP32 receives the result and:
   - Moves **servo1** to 90° and back to 0° (drop gate, ~2 s total).
   - Turns the **conveyor relay** ON.
     - If `DEFECTED`: conveyor runs 5 s, then OFF.
     - If `NORMAL` and weight `> 100 g`: **servo2** moves to 45° while the
       conveyor runs 5 s, then conveyor OFF and servo2 back to 0°.
     - If `NORMAL` and weight `<= 100 g`: **servo3** moves to 45° while the
       conveyor runs 5 s, then conveyor OFF and servo3 back to 0°.
6. The ESP32 waits until the load cell reads empty again (orange has left
   the platform), then goes back to step 1. The laptop is idle/listening the
   whole time, ready for the next `OBJECT_DETECTED` message. This repeats
   forever.

> Note: the original spec mentioned both "2 secs" and "3 secs" for the
> rotation motor. This build uses **3 seconds** (`ROTATE_SECONDS` in
> `app.py`) since that's what completes one full rotation for inspection —
> change it in one place if you want a different duration.

## MQTT topics

Both boards connect to the same public broker, so pick topic names that
aren't likely to collide with other public users — the defaults below use
a project-specific prefix.

| Direction | Topic | Payload | Meaning |
|---|---|---|---|
| ESP32 → laptop | `orangesort/detect` | `OBJECT_DETECTED` | Orange placed on platform |
| Laptop → ESP32 | `orangesort/motor` | `ON` or `OFF` | Rotation motor relay |
| Laptop → ESP32 | `orangesort/result` | `DEFECTED` or `NORMAL` | Inspection result |

The ESP32 only obeys `ON` while an orange is waiting for inspection, and
switches the rotation motor off by itself after 8 s
(`MOTOR_MAX_ON_MS`) if the laptop never sends `OFF`.

Broker: `broker.hivemq.com`, port `1883` (plain MQTT, no login needed).

## Hardware wiring

### ESP32

| Signal | GPIO |
|---|---|
| HX711 DOUT | 16 |
| HX711 SCK | 4 |
| Conveyor relay IN | 17 |
| Rotation motor relay IN | 5 (active LOW) |
| Servo1 (drop gate) signal | 18 |
| Servo2 (heavy path) signal | 19 |
| Servo3 (light path) signal | 21 |

### Laptop

| Signal | Connection |
|---|---|
| Camera | Built-in webcam or USB camera (`CAMERA_INDEX` in `app.py`) |

**Power notes:**
- Drive the conveyor/rotation motors through the relays, not directly
  from ESP32 pins — use a separate motor power supply and share a
  common ground with the boards.
- The conveyor relay is active-HIGH and the rotation relay is active-LOW
  by default. If yours differ, change `CONVEYOR_RELAY_ACTIVE_HIGH` /
  `MOTOR_RELAY_ACTIVE_HIGH` in `esp32_code.ino`.
- Power servos from a 5V supply capable of a few hundred mA each, not
  directly from the ESP32 3V3/5V pin if you have more than one under load.
- For a steady weight reading: power the HX711 from the ESP32 **3V3** pin
  (not a separate supply), add a 100 µF + 100 nF capacitor across the
  HX711 VCC/GND, keep the load cell wires short and away from the motor
  and relay wires, and make sure the platform only touches the load cell
  (nothing rubbing on the frame).

## Setup

### 1. Laptop (`app.py`)

Install Python 3.9+ and get the code:

```powershell
git clone https://github.com/iamaftab18/orange-defect-detection.git
cd orange-defect-detection
python -m venv venv
venv\Scriptsctivate
pip install -r requirements.txt
```

(macOS/Linux: `source venv/bin/activate` instead of the Windows activate
line.) `requirements.txt` installs `opencv-python` (the build with window
support). If `opencv-python-headless` is installed, uninstall it first
because it cannot open windows.

Make sure the laptop is online (it reaches HiveMQ over the internet; it
does not need to be on the same WiFi as the ESP32). Then run:

```powershell
python app.py
```

It connects to HiveMQ, subscribes to `orangesort/detect`, opens the live
video window, and prints status as oranges are processed. Leave it
running — it loops forever. Press `q` in the video window (or `Ctrl+C`
in the terminal) to quit.

## Live video window

The window is a 3 × 2 grid, updated live from the laptop camera:

| Tile | What it shows |
|---|---|
| Original | Raw camera image |
| Grayscale | Camera image converted to gray |
| HSV | Camera image converted to the HSV color space |
| Orange filter | Only the orange-colored pixels (`ORANGE_HSV_LOWER/UPPER`) |
| Black filter | White where pixels are black (`V < BLACK_VALUE_THRESHOLD`) |
| Detection | Original with red outlines around black patches bigger than `BLACK_AREA_THRESHOLD`, plus the current status |

The status line at the bottom of the Detection tile is `WAITING FOR
ORANGE`, `INSPECTING x.x/3s` while the motor is rotating, then `RESULT:
DEFECTED` / `RESULT: NORMAL` for `RESULT_HOLD_SECONDS`. The window is
also handy for tuning: watch the Black filter and Detection tiles while
you adjust the thresholds.

Set `SHOW_VIDEO = False` in `app.py` to run without the window.

### 2. ESP32 (`esp32_code.ino`)

1. Open `esp32_code.ino` in the Arduino IDE (with ESP32 board support
   installed).
2. Install libraries via **Sketch → Include Library → Manage Libraries**:
   - `PubSubClient` (Nick O'Leary)
   - `HX711` (bogde)
   - `ESP32Servo` (Kevin Harrington / Dlloydev)
3. WiFi credentials are already set to:
   - SSID: `network`
   - Password: `1122334455`
   (edit `WIFI_SSID` / `WIFI_PASSWORD` at the top of the file if these
   change).
4. Upload to the ESP32 and open the Serial Monitor at `115200` baud
   with the line ending set to **Newline**. You will see the WiFi/MQTT
   connection, then `Weight: x.x g` once a second.

#### Load cell calibration

The load cell is already calibrated: `CALIBRATION_FACTOR` in
`esp32_code.ino` is `153.8`. It was worked out on this machine from a
98 g object: with a trial factor of `-688.033` the empty platform read
`+0.4 g` and the object read `-21.5 g`, which gives
`-688.033 × (-21.9 ÷ 98) = 153.8`. There is nothing to calibrate when you
first upload. (The `-688033` factor from the separate gas-monitor sketch
does not fit this platform, so don't reuse it.)

- The scale is zeroed automatically at every start-up, after WiFi/MQTT
  is connected, and again after each orange leaves the platform. Keep the
  platform empty while the ESP32 boots. Type `tare` in the Serial Monitor
  (line ending **Newline**) to zero it manually.
- If you change the load cell, the platform, or the HX711 wiring,
  recalibrate: put a known weight on the platform and note what the
  Serial Monitor shows. Then set
  `CALIBRATION_FACTOR = old factor × (shown grams ÷ real grams)`.
  Read the weight after the platform has been zeroed with `tare`. For
  example, a 200 g weight showing `190 g` with the factor `153.8` gives
  `153.8 × 190 ÷ 200 = 146.1`.
- If the empty platform still drifts or jumps by tens of grams, that is
  a wiring/power/mechanical problem, not a calibration problem. See the
  troubleshooting section.

## Tuning defect detection

In `app.py`:

- `BLACK_VALUE_THRESHOLD` (default 60) — HSV "Value" below this counts as
  black. Lower it if your background/shadows are being misread as
  defects; raise it if real black patches are being missed.
- `BLACK_AREA_THRESHOLD` (default 800 px) — how big a black contour has
  to be to count as a defect. Depends on camera resolution/distance;
  increase it if small shadows/noise trigger false positives.
- `FRAME_SAMPLES` (default 6) — how many frames are checked during the
  one rotation. More samples = better coverage of the orange's surface,
  at the cost of a bit more CPU per cycle.

## Troubleshooting

- **ESP32 won't connect to WiFi**: double check SSID/password and that
  it's a 2.4 GHz network (ESP32 doesn't support 5 GHz).
- **MQTT messages not arriving**: `broker.hivemq.com` is a shared public
  broker — if it's unreachable or slow, try again after a minute, or
  point both `app.py` and `esp32_code.ino` at another broker/port.
- **Weight shows tens or hundreds of grams with nothing on the platform**:
  1. Keep the platform empty while the ESP32 boots, then type `tare`.
  2. If it still wanders, check the hardware: HX711 VCC on the ESP32
     **3V3** pin with a 100 µF + 100 nF capacitor, short wires away from
     motors/relays, all four load cell wires firmly connected, and the
     platform not touching the frame (a rubbing platform or a loose
     mounting screw gives a changing offset). Also confirm the load cell
     arrow / mounting direction matches the direction of the load.
- **Weight reads negative or the wrong way round**: swapped load cell
  signal wires flip the sign. Change the sign of `CALIBRATION_FACTOR` in
  `esp32_code.ino`.
- **`HX711 not found` keeps printing**: check the DOUT (GPIO 16), SCK
  (GPIO 4), VCC and GND wires to the HX711 board.
- **No video window appears** — `This OpenCV build has no window
  support` means `opencv-python-headless` is installed; run
  `pip uninstall -y opencv-python-headless` then
  `pip install --force-reinstall opencv-python`.
- **Camera not found** (`Could not open camera index 0`): close other apps
  using the camera (Teams, Zoom, browser) and try `CAMERA_INDEX = 1` or
  `2` in `app.py` (0 is usually the built-in webcam, external USB cameras
  come after it).
- **Rotation motor doesn't spin / spins the wrong way round**: run
  `python test.py` with a weight on the load cell to check the relay. If
  ON/OFF is inverted, flip `MOTOR_RELAY_ACTIVE_HIGH` in `esp32_code.ino`.
