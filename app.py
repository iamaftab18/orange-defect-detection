"""
Orange Defect Detection - Raspberry Pi application.

Waits for the ESP32 to report that an orange has been placed on the
platform (via MQTT), spins the rotation motor with a relay so the
camera can see the whole surface of the orange, looks for big black
patches (defects) using simple HSV color filtering, then reports
"DEFECTED" or "NORMAL" back to the ESP32 over MQTT.

Runs forever in a loop, one orange at a time.
"""

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
RELAY_ACTIVE_HIGH = True

# How long the platform motor spins to complete one rotation
ROTATE_SECONDS = 3

# How many camera frames to sample while the orange is rotating
FRAME_SAMPLES = 6

# USB webcam device index (0 = /dev/video0). Check `ls /dev/video*`.
CAMERA_INDEX = 0

# --- Defect detection tuning ---
# A pixel is considered "black" when its HSV Value is below this.
BLACK_VALUE_THRESHOLD = 60
# A contour of black pixels bigger than this (in pixels) counts as a
# "big" defect patch. Tune this based on your camera resolution/distance.
BLACK_AREA_THRESHOLD = 800

# ------------------------------------------------------------------
# GPIO setup
# ------------------------------------------------------------------

GPIO.setmode(GPIO.BCM)
GPIO.setup(MOTOR_RELAY_PIN, GPIO.OUT)


def motor_on():
    GPIO.output(MOTOR_RELAY_PIN, GPIO.HIGH if RELAY_ACTIVE_HIGH else GPIO.LOW)


def motor_off():
    GPIO.output(MOTOR_RELAY_PIN, GPIO.LOW if RELAY_ACTIVE_HIGH else GPIO.HIGH)


motor_off()

camera = cv2.VideoCapture(CAMERA_INDEX)

# Set True while an orange is being processed, so a second
# "OBJECT_DETECTED" message can't interrupt the current cycle.
busy = False


# ------------------------------------------------------------------
# Defect detection
# ------------------------------------------------------------------

def frame_has_black_patch(frame):
    """Return True if the frame contains a big black patch (defect)."""
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    black_mask = cv2.inRange(hsv, (0, 0, 0), (180, 255, BLACK_VALUE_THRESHOLD))

    contours, _ = cv2.findContours(
        black_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
    )
    if not contours:
        return False

    largest_area = max(cv2.contourArea(c) for c in contours)
    return largest_area > BLACK_AREA_THRESHOLD


def read_fresh_frame():
    """Drop buffered (stale) frames so we analyse what the camera sees now."""
    for _ in range(4):
        camera.grab()
    return camera.read()


def rotate_and_inspect():
    """Spin the platform motor and sample frames for black defect patches."""
    defected = False
    interval = ROTATE_SECONDS / FRAME_SAMPLES

    motor_on()
    for _ in range(FRAME_SAMPLES):
        ok, frame = read_fresh_frame()
        if ok and frame_has_black_patch(frame):
            defected = True
        time.sleep(interval)
    motor_off()

    return defected


# ------------------------------------------------------------------
# MQTT callbacks
# ------------------------------------------------------------------

def on_connect(client, userdata, flags, rc):
    print(f"Connected to {MQTT_BROKER} (rc={rc})")
    client.subscribe(TOPIC_DETECT)
    print(f"Subscribed to '{TOPIC_DETECT}', waiting for oranges...")


def on_message(client, userdata, msg):
    global busy

    payload = msg.payload.decode(errors="ignore")
    print(f"[{msg.topic}] {payload}")

    if msg.topic != TOPIC_DETECT or busy:
        return

    busy = True
    print("Orange detected -> rotating and inspecting...")

    defected = rotate_and_inspect()
    result = "DEFECTED" if defected else "NORMAL"

    print(f"Result: {result}")
    client.publish(TOPIC_RESULT, result)

    busy = False


# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------

def main():
    if not camera.isOpened():
        motor_off()
        GPIO.cleanup()
        raise SystemExit(
            f"Could not open camera index {CAMERA_INDEX}. "
            "Check `ls /dev/video*` and set CAMERA_INDEX in app.py."
        )

    # Let the USB camera's auto-exposure settle; first frames are often dark.
    for _ in range(10):
        camera.read()

    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("Stopping...")
    finally:
        motor_off()
        camera.release()
        GPIO.cleanup()


if __name__ == "__main__":
    main()
