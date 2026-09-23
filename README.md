# Orange Defect Detection (ESP32 + Raspberry Pi)

A simple two-board sorting machine:

- **ESP32** — reads a load cell to detect when an orange is placed on the
  platform, and drives the conveyor relay + 3 servos to route it.
- **Raspberry Pi** — spins the rotation motor, looks at the orange with a
  camera, and decides `DEFECTED` or `NORMAL` using basic color/black-patch
  detection.
- **MQTT** (HiveMQ free public broker `broker.hivemq.com`) is the glue
  between the two boards — no local broker to set up.

Files in this project:

| File | Runs on | Purpose |
|---|---|---|
| `app.py` | Raspberry Pi | MQTT client, motor relay control, camera defect detection |
| `esp32_code.ino` | ESP32 | WiFi/MQTT, load cell, conveyor relay, 3 servos |
| `README.md` | — | This file |

## How it works

1. Orange is placed on the platform. The ESP32's load cell (HX711) detects
   weight above `WEIGHT_PRESENT_THRESHOLD_G` (default 5 g), waits until
   the reading stops changing (so a half-placed orange isn't weighed),
   and publishes `OBJECT_DETECTED` on MQTT topic `orangesort/detect`.
2. The Raspberry Pi receives that message, turns on the **rotation motor
   relay** for `ROTATE_SECONDS` (default 3 s, one full rotation), and
   samples camera frames while it spins. A live window shows the camera
   through several filters the whole time (see "Live video window").
3. Each frame is converted to HSV and checked for a big patch of black
   pixels (rot/mold typically looks like a large dark/black blotch). If
   any sampled frame has a black patch bigger than `BLACK_AREA_THRESHOLD`,
   the orange is marked defected.
4. The Pi stops the motor and publishes the result (`DEFECTED` or
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
   the platform), then goes back to step 1. The Pi is idle/listening the
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
| ESP32 → Pi | `orangesort/detect` | `OBJECT_DETECTED` | Orange placed on platform |
| Pi → ESP32 | `orangesort/result` | `DEFECTED` or `NORMAL` | Inspection result |

Broker: `broker.hivemq.com`, port `1883` (plain MQTT, no login needed).

## Hardware wiring

### ESP32

| Signal | GPIO |
|---|---|
| HX711 DOUT | 16 |
| HX711 SCK | 4 |
| Conveyor relay IN | 17 |
| Servo1 (drop gate) signal | 18 |
| Servo2 (heavy path) signal | 19 |
| Servo3 (light path) signal | 21 |

### Raspberry Pi

| Signal | GPIO (BCM) |
|---|---|
| Rotation motor relay IN | 27 |
| Camera | USB webcam (`/dev/video0`) |

**Power notes:**
- Drive the conveyor/rotation motors through the relays, not directly
  from ESP32/Pi pins — use a separate motor power supply and share a
  common ground with the boards.
- Most cheap relay modules are active-HIGH (pin HIGH = relay ON). If
  yours is active-LOW, set `RELAY_ACTIVE_HIGH = false` in `esp32_code.ino`
  and `RELAY_ACTIVE_HIGH = False` in `app.py`.
- Power servos from a 5V supply capable of a few hundred mA each, not
  directly from the ESP32 3V3/5V pin if you have more than one under load.
- For a steady weight reading: power the HX711 from the ESP32 **3V3** pin
  (not a separate supply), add a 100 µF + 100 nF capacitor across the
  HX711 VCC/GND, keep the load cell wires short and away from the motor
  and relay wires, and make sure the platform only touches the load cell
  (nothing rubbing on the frame).

## Setup

### 1. Raspberry Pi (`app.py`)

Get the code (first time only), or update it later:

```bash
# first time
git clone https://github.com/iamaftab18/orange-defect-detection.git
cd orange-defect-detection

# later, to get the latest changes
cd orange-defect-detection
git pull
```

Install dependencies into a virtual environment. GPIO comes from the
`python3-rpi-lgpio` apt package, so nothing needs to be compiled:

```bash
sudo apt update
sudo apt remove -y python3-rpi.gpio
sudo apt install -y python3-venv python3-rpi-lgpio python3-lgpio
python3 -m venv --system-site-packages venv
source venv/bin/activate
pip install -r requirements.txt
```

Check that GPIO works before running the app (should print `GPIO OK`):

```bash
python -c "import RPi.GPIO as G; G.setmode(G.BCM); G.setup(27, G.OUT); print('GPIO OK')"
```

(Run `source venv/bin/activate` again in every new terminal before
running `app.py`, and do not use `sudo python3 app.py` — sudo ignores
the virtual environment.)

> Why `rpi-lgpio`: it provides the same `import RPi.GPIO` API as the old
> `RPi.GPIO` but also works on Raspberry Pi 5 and current Raspberry Pi
> OS. The old `RPi.GPIO` fails there with
> `RuntimeError: Cannot determine SOC peripheral base address`. The two
> packages cannot be installed in the same Python environment, so if you
> hit that error you still have the old one: run `pip uninstall -y
> RPi.GPIO` inside the venv, remove the `python3-rpi.gpio` apt package,
> or recreate the venv with the commands above (`rm -rf venv` first).

Plug in the USB webcam before starting. This app uses a USB webcam only
(the Pi Camera Module is not supported by `cv2.VideoCapture`).

`requirements.txt` installs `opencv-python` (the build with window
support). If you installed `opencv-python-headless` earlier, remove it
first, because it cannot open windows:

```bash
deactivate 2>/dev/null
python3 -m pip uninstall -y opencv-python-headless --break-system-packages
source venv/bin/activate
pip install -r requirements.txt
```

Run it **from the Pi's own desktop terminal (or VNC)** so the video
window has a screen to appear on:

```bash
python app.py
```

It will connect to HiveMQ, subscribe to `orangesort/detect`, open the
live video window, and print status as oranges are processed. Leave it
running — it loops forever. Press `q` in the video window (or `Ctrl+C`
in the terminal) to quit.

## Live video window

The window is a 3 × 2 grid, updated live from the USB webcam:

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

Set `SHOW_VIDEO = False` in `app.py` to run without the window. If no
screen is available (for example over plain SSH), the window is turned
off automatically and the app keeps working.

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
5. **Calibrate the load cell** (see below). This is needed once.

#### Calibrating the load cell

The built-in default calibration factor is only a placeholder, so the
weight will be wrong until you calibrate. You need one object of known
weight (for example a 100 g or 200 g calibration weight, or anything you
have weighed on a kitchen scale).

1. Type `cal` in the Serial Monitor and press Enter.
2. Follow the prompts:
   1. Remove everything from the platform, press Enter. The scale zeroes.
   2. Put the known weight on the platform, wait a few seconds, type its
      weight in grams (for example `100`) and press Enter.
   3. The ESP32 prints the calculated factor, saves it in flash (it
      survives power-off), and shows a check reading. Remove the weight
      and press Enter.
3. The empty platform should now read about `0.0 g`, and the known
   weight should read correctly.

Other things to know:

- The scale is zeroed automatically at every start-up, after WiFi/MQTT
  is connected, and again after each orange leaves the platform. Keep the
  platform empty while the ESP32 boots. Type `tare` to zero it manually.
- Calibration is normally done once. Repeat it if you change the load
  cell, the platform, or the HX711 wiring.
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
  2. Run `cal` (the placeholder factor is wrong for most load cells).
  3. If it still wanders, check the hardware: HX711 VCC on the ESP32
     **3V3** pin with a 100 µF + 100 nF capacitor, short wires away from
     motors/relays, all four load cell wires firmly connected, and the
     platform not touching the frame (a rubbing platform or a loose
     mounting screw gives a changing offset). Also confirm the load cell
     arrow / mounting direction matches the direction of the load.
- **Weight reads negative or the wrong way round**: the calibration
  handles swapped signal wires automatically, so run `cal` again.
- **`HX711 not found` keeps printing**: check the DOUT (GPIO 16), SCK
  (GPIO 4), VCC and GND wires to the HX711 board.
- **No video window appears** — check the message printed at startup:
  `No display found (running over SSH?)` means the app was started
  without a screen (run it from the Pi desktop or VNC).
  `This OpenCV build has no window support` means
  `opencv-python-headless` is installed (see the Setup section to
  replace it with `opencv-python`).
- **Camera not found on the Pi** (`Could not open camera index 0`): run
  `ls /dev/video*` with the webcam plugged in. If the webcam is not
  `/dev/video0` (a Pi 5 lists extra internal video devices), set
  `CAMERA_INDEX` in `app.py` to its number. `v4l2-ctl --list-devices`
  (from `sudo apt install v4l-utils`) shows which device is the webcam.
