"""
Orange Defect Detection - Raspberry Pi application.

Waits for the ESP32 to report that an orange has been placed on the
platform (via MQTT), spins the rotation motor with a relay so the
camera can see the whole surface of the orange, looks for big black
patches (defects) using simple HSV color filtering, then reports
"DEFECTED" or "NORMAL" back to the ESP32 over MQTT.

A live window shows the camera footage through several filters
(original, grayscale, HSV, orange filter, black filter, detection).
Press "q" in that window to quit.

Runs forever in a loop, one orange at a time.
"""

import os
import threading
import time

import cv2
import paho.mqtt.client as mqtt
import RPi.GPIO as GPIO

# ------------------------------------------------------------------
# Configuration - change these to match your hardware / broker
# ------------------------------------------------------------------

# HiveMQ public broker (free, no auth needed)
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
MQTT_CLIENT_ID = "raspberrypi-orange-sorter"

# MQTT topics shared with the ESP32
TOPIC_DETECT = "orangesort/detect"   # ESP32 -> Pi : object placed on platform
TOPIC_RESULT = "orangesort/result"   # Pi -> ESP32 : "DEFECTED" or "NORMAL"

# Rotation motor relay (BCM numbering). Set RELAY_ACTIVE_HIGH = False
# if your relay module switches ON when the pin is LOW.
MOTOR_RELAY_PIN = 27
RELAY_ACTIVE_HIGH = False

# How long the platform motor spins to complete one rotation
ROTATE_SECONDS = 3

# How many camera frames to check for defects while the orange rotates
FRAME_SAMPLES = 6

# USB webcam device index (0 = /dev/video0). Check `ls /dev/video*`.
CAMERA_INDEX = 0
CAMERA_WIDTH = 640
CAMERA_HEIGHT = 480

# --- Live video window (needs a screen: Pi desktop or VNC, not plain SSH) ---
SHOW_VIDEO = True
WINDOW_NAME = "Orange Defect Detection (press q to quit)"
TILE_WIDTH = 320            # width of each filter view inside the window
RESULT_HOLD_SECONDS = 3     # how long the last result stays on screen

# Orange filter (display only): HSV range of orange skin
ORANGE_HSV_LOWER = (5, 100, 100)
ORANGE_HSV_UPPER = (25, 255, 255)

# --- Defect detection tuning ---
# A pixel is considered "black" when its HSV Value is below this.
BLACK_VALUE_THRESHOLD = 60
# A contour of black pixels bigger than this (in pixels) counts as a
# "big" defect patch. Tune this based on your camera resolution/distance.
BLACK_AREA_THRESHOLD = 800

# BGR colors used for on-screen text
WHITE = (255, 255, 255)
YELLOW = (0, 255, 255)
RED = (0, 0, 255)
GREEN = (0, 255, 0)

# ------------------------------------------------------------------
# GPIO setup
# ------------------------------------------------------------------

GPIO.setmode(GPIO.BCM)
# Start at the OFF level so an active-low relay doesn't click on at startup.
GPIO.setup(MOTOR_RELAY_PIN, GPIO.OUT,
           initial=GPIO.LOW if RELAY_ACTIVE_HIGH else GPIO.HIGH)


def motor_on():
    GPIO.output(MOTOR_RELAY_PIN, GPIO.HIGH if RELAY_ACTIVE_HIGH else GPIO.LOW)


def motor_off():
    GPIO.output(MOTOR_RELAY_PIN, GPIO.LOW if RELAY_ACTIVE_HIGH else GPIO.HIGH)


motor_off()

# Set by the MQTT thread when the ESP32 reports an orange on the platform.
detect_requested = threading.Event()


# ------------------------------------------------------------------
# Defect detection
# ------------------------------------------------------------------

def find_black_patches(frame):
    """Return (hsv, black_mask, big_contours) for a BGR camera frame."""
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    black_mask = cv2.inRange(hsv, (0, 0, 0), (180, 255, BLACK_VALUE_THRESHOLD))

    contours, _ = cv2.findContours(
        black_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
    )
    big_contours = [c for c in contours if cv2.contourArea(c) > BLACK_AREA_THRESHOLD]
    return hsv, black_mask, big_contours


# ------------------------------------------------------------------
# Live video window
# ------------------------------------------------------------------

def put_text(image, text, origin, color=YELLOW, scale=0.55):
    """Draw text on a dark box so it is readable on any background."""
    font = cv2.FONT_HERSHEY_SIMPLEX
    (text_w, text_h), baseline = cv2.getTextSize(text, font, scale, 1)
    x, y = origin
    cv2.rectangle(
        image, (x - 4, y - text_h - 4), (x + text_w + 4, y + baseline + 2), (0, 0, 0), -1
    )
    cv2.putText(image, text, origin, font, scale, color, 1, cv2.LINE_AA)


def make_tile(image, label, size, color=YELLOW):
    if image.ndim == 2:
        image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    tile = cv2.resize(image, size)
    put_text(tile, label, (8, 20), color)
    return tile


def build_display(frame, hsv, black_mask, big_contours, status_text, status_color):
    """Combine the camera view and its filters into one 3x2 picture."""
    height, width = frame.shape[:2]
    size = (TILE_WIDTH, int(TILE_WIDTH * height / width))

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    orange_mask = cv2.inRange(hsv, ORANGE_HSV_LOWER, ORANGE_HSV_UPPER)
    orange_only = cv2.bitwise_and(frame, frame, mask=orange_mask)

    detection = frame.copy()
    cv2.drawContours(detection, big_contours, -1, RED, 3)
    if big_contours:
        detection_label, detection_color = "Detection: BLACK PATCH", RED
    else:
        detection_label, detection_color = "Detection: clear", GREEN

    tiles = [
        make_tile(frame, "Original", size),
        make_tile(gray, "Grayscale", size),
        make_tile(hsv, "HSV", size),
        make_tile(orange_only, "Orange filter", size),
        make_tile(black_mask, f"Black filter (V<{BLACK_VALUE_THRESHOLD})", size),
        make_tile(detection, detection_label, size, detection_color),
    ]
    put_text(tiles[5], status_text, (8, size[1] - 10), status_color, 0.6)

    return cv2.vconcat([cv2.hconcat(tiles[:3]), cv2.hconcat(tiles[3:])])


# ------------------------------------------------------------------
# MQTT callbacks
# ------------------------------------------------------------------

def on_connect(client, userdata, flags, rc):
    print(f"Connected to {MQTT_BROKER} (rc={rc})")
    client.subscribe(TOPIC_DETECT)
    print(f"Subscribed to '{TOPIC_DETECT}', waiting for oranges...")


def on_message(client, userdata, msg):
    payload = msg.payload.decode(errors="ignore")
    print(f"[{msg.topic}] {payload}")

    if msg.topic == TOPIC_DETECT:
        detect_requested.set()


# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------

def main():
    camera = cv2.VideoCapture(CAMERA_INDEX)
    if not camera.isOpened():
        motor_off()
        GPIO.cleanup()
        raise SystemExit(
            f"Could not open camera index {CAMERA_INDEX}. "
            "Check `ls /dev/video*` and set CAMERA_INDEX in app.py."
        )
    camera.set(cv2.CAP_PROP_FRAME_WIDTH, CAMERA_WIDTH)
    camera.set(cv2.CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT)
    camera.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    show_video = SHOW_VIDEO
    has_display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    if show_video and os.name == "posix" and not has_display:
        print("No display found (running over SSH?) - video window disabled.")
        show_video = False

    # Let the USB camera's auto-exposure settle; first frames are often dark.
    for _ in range(10):
        camera.read()

    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect_async(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()

    sample_interval = ROTATE_SECONDS / FRAME_SAMPLES
    inspecting = False
    inspect_start = 0.0
    samples_taken = 0
    defected = False
    last_result = ""
    last_result_time = 0.0

    try:
        while True:
            ok, frame = camera.read()
            now = time.monotonic()

            big_contours = []
            if ok:
                hsv, black_mask, big_contours = find_black_patches(frame)

            # --- inspection cycle: start, sample frames, finish ---
            if not inspecting:
                if detect_requested.is_set():
                    detect_requested.clear()
                    print("Orange detected -> rotating and inspecting...")
                    motor_on()
                    inspecting = True
                    inspect_start = now
                    samples_taken = 0
                    defected = False
            else:
                detect_requested.clear()  # ignore new requests while busy
                elapsed = now - inspect_start

                due = samples_taken * sample_interval
                if samples_taken < FRAME_SAMPLES and elapsed >= due:
                    if big_contours:
                        defected = True
                    samples_taken += 1

                if elapsed >= ROTATE_SECONDS:
                    motor_off()
                    last_result = "DEFECTED" if defected else "NORMAL"
                    last_result_time = now
                    print(f"Result: {last_result}")
                    client.publish(TOPIC_RESULT, last_result)
                    inspecting = False

            if not ok:
                time.sleep(0.05)
                continue

            # --- live window ---
            if show_video:
                if inspecting:
                    status_text = f"INSPECTING {now - inspect_start:.1f}/{ROTATE_SECONDS}s"
                    status_color = YELLOW
                elif last_result and now - last_result_time < RESULT_HOLD_SECONDS:
                    status_text = f"RESULT: {last_result}"
                    status_color = RED if last_result == "DEFECTED" else GREEN
                else:
                    status_text = "WAITING FOR ORANGE"
                    status_color = WHITE

                display = build_display(
                    frame, hsv, black_mask, big_contours, status_text, status_color
                )
                try:
                    cv2.imshow(WINDOW_NAME, display)
                    key = cv2.waitKey(1) & 0xFF
                except cv2.error:
                    print(
                        "This OpenCV build has no window support, video window "
                        "disabled. Fix: pip uninstall opencv-python-headless "
                        "&& pip install opencv-python"
                    )
                    show_video = False
                    key = -1
                if key == ord("q"):
                    break
    except KeyboardInterrupt:
        print("Stopping...")
    finally:
        motor_off()
        client.loop_stop()
        client.disconnect()
        camera.release()
        GPIO.cleanup()
        if show_video:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
