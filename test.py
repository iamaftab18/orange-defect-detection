"""Relay test for the rotation motor relay (wired to the ESP32).

Sends MQTT "ON" then "OFF" to the ESP32 motor topic every 2 s, forever
(Ctrl+C to stop). The ESP32 only spins the motor while an orange is waiting
for inspection, so first put a weight (>5 g) on the load cell and make sure
app.py is NOT running. (The ESP32 also has an 8 s safety cut-off.)

Watch the relay (click sound / LED): it should be ON while "ON" is sent.
If it is inverted, flip MOTOR_RELAY_ACTIVE_HIGH in esp32_code.ino.

Usage:  python test.py
"""

import time

import paho.mqtt.client as mqtt

MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
TOPIC_MOTOR = "orangesort/motor"
STEP_SECONDS = 2

client = mqtt.Client(client_id="laptop-orange-relay-test")
client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
client.loop_start()

print("Testing rotation motor relay via MQTT. Ctrl+C to stop.")
try:
    while True:
        client.publish(TOPIC_MOTOR, "ON")
        print(f"Sent ON  - relay should be ON for {STEP_SECONDS}s")
        time.sleep(STEP_SECONDS)

        client.publish(TOPIC_MOTOR, "OFF")
        print(f"Sent OFF - relay should be OFF for {STEP_SECONDS}s")
        time.sleep(STEP_SECONDS)
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    client.publish(TOPIC_MOTOR, "OFF")
    time.sleep(0.3)
    client.loop_stop()
    client.disconnect()
