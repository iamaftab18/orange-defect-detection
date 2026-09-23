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
   weight above `WEIGHT_PRESENT_THRESHOLD_G` (default 5 g) and publishes
   `OBJECT_DETECTED` on MQTT topic `orangesort/detect`.
2. The Raspberry Pi receives that message, turns on the **rotation motor
   relay** for `ROTATE_SECONDS` (default 3 s, one full rotation), and
   samples camera frames while it spins.
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
| Camera | USB webcam / Pi camera via `/dev/video0` |

**Power notes:**
- Drive the conveyor/rotation motors through the relays, not directly
  from ESP32/Pi pins — use a separate motor power supply and share a
  common ground with the boards.
- Most cheap relay modules are active-HIGH (pin HIGH = relay ON). If
  yours is active-LOW, set `RELAY_ACTIVE_HIGH = false` in `esp32_code.ino`
  and `RELAY_ACTIVE_HIGH = False` in `app.py`.
- Power servos from a 5V supply capable of a few hundred mA each, not
  directly from the ESP32 3V3/5V pin if you have more than one under load.

## Setup

### 1. Raspberry Pi (`app.py`)

Install dependencies:

```bash
sudo apt update
sudo apt install -y python3-opencv
pip3 install paho-mqtt RPi.GPIO
```

If you're using the Raspberry Pi Camera Module (not a USB webcam), enable
the V4L2 compatibility layer so `cv2.VideoCapture(0)` can see it:

```bash
sudo modprobe bcm2835-v4l2
```

Run it:

```bash
python3 app.py
```

It will connect to HiveMQ, subscribe to `orangesort/detect`, and print
status as oranges are processed. Leave it running — it loops forever.

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
4. **Calibrate the load cell**: the default `LOADCELL_CALIBRATION_FACTOR`
   (2280.0) is a placeholder. Run a basic HX711 calibration sketch with a
   known weight first, then update the constant in `esp32_code.ino`.
5. Upload to the ESP32 and open the Serial Monitor at `115200` baud to
   watch WiFi/MQTT connection and weight readings.

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
- **Load cell reads negative/unstable**: re-run `scale.tare()` with the
  platform empty, and confirm `LOADCELL_CALIBRATION_FACTOR` is correct
  for your specific cell.
- **Camera not found on the Pi**: confirm `ls /dev/video0` exists; for
  the Pi Camera Module make sure `bcm2835-v4l2` is loaded (see setup
  above), or change `CAMERA_INDEX` in `app.py`.
