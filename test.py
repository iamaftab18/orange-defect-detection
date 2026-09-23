"""Relay test for the rotation motor relay (Raspberry Pi).

Drives the relay pin HIGH for 2 s, then LOW for 2 s, forever (Ctrl+C to stop).
Watch the relay (click sound / LED) and note which level turns it ON:

  * relay ON while the pin is HIGH -> active HIGH -> RELAY_ACTIVE_HIGH = True
  * relay ON while the pin is LOW  -> active LOW  -> RELAY_ACTIVE_HIGH = False

Usage:  python test.py [BCM_PIN]      (default pin 27, same as app.py)
"""

import sys
import time

import RPi.GPIO as GPIO

MOTOR_RELAY_PIN = int(sys.argv[1]) if len(sys.argv) > 1 else 27
STEP_SECONDS = 2

GPIO.setmode(GPIO.BCM)
GPIO.setup(MOTOR_RELAY_PIN, GPIO.OUT, initial=GPIO.LOW)

print(f"Testing relay on BCM GPIO {MOTOR_RELAY_PIN}. Ctrl+C to stop.")
print("Watch the relay: which pin level turns it ON?\n")

try:
    while True:
        GPIO.output(MOTOR_RELAY_PIN, GPIO.HIGH)
        print(f"Pin HIGH (3.3V) for {STEP_SECONDS}s  -> relay ON here = active HIGH")
        time.sleep(STEP_SECONDS)

        GPIO.output(MOTOR_RELAY_PIN, GPIO.LOW)
        print(f"Pin LOW  (0V)   for {STEP_SECONDS}s  -> relay ON here = active LOW")
        time.sleep(STEP_SECONDS)
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    GPIO.cleanup()
