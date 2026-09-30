/*
  Orange Defect Detection - ESP32 application.

  - Reads a load cell (HX711) to detect when an orange has been placed
    on the platform, and tells the laptop over MQTT.
  - Runs the rotation motor relay when the laptop says so (the laptop
    camera inspects the orange while it spins).
  - Waits for the laptop to report "DEFECTED" or "NORMAL".
  - Moves servo1 (drop gate) 90 degrees and back, then turns on the
    conveyor relay and routes the orange using servo2 (normal + heavy)
    or servo3 (normal + light), or just the conveyor alone if defected.
  - Runs forever, one orange at a time.

  Serial Monitor (115200 baud) command:
    tare  - zero the scale (platform must be empty)

  Required libraries (install via Arduino Library Manager):
    - PubSubClient      (by Nick O'Leary)
    - HX711             (by bogde)
    - ESP32Servo        (by Kevin Harrington / Dlloydev)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HX711.h>
#include <ESP32Servo.h>

// ------------------------------------------------------------------
// WiFi / MQTT configuration
// ------------------------------------------------------------------

const char *WIFI_SSID = "network";
const char *WIFI_PASSWORD = "1122334455";

const char *MQTT_BROKER = "broker.hivemq.com";  // HiveMQ free public broker
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "esp32-orange-sorter";

const char *TOPIC_DETECT = "orangesort/detect";  // ESP32 -> laptop
const char *TOPIC_MOTOR = "orangesort/motor";    // laptop -> ESP32: "ON" / "OFF"
const char *TOPIC_RESULT = "orangesort/result";  // laptop -> ESP32: "DEFECTED" / "NORMAL"

// ------------------------------------------------------------------
// Pin configuration
// ------------------------------------------------------------------

const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN = 4;

const int RELAY_CONVEYOR_PIN = 17;  // conveyor belt motor relay
const int RELAY_MOTOR_PIN = 5;      // rotation motor relay (platform)
const int SERVO1_PIN = 18;          // drop gate, right after rotation
const int SERVO2_PIN = 19;          // normal + heavy (>100g) path
const int SERVO3_PIN = 21;          // normal + light (<=100g) path

const bool CONVEYOR_RELAY_ACTIVE_HIGH = true;  // set false if your relay turns ON on LOW
const bool MOTOR_RELAY_ACTIVE_HIGH = false;    // rotation relay is active LOW

// Safety: if the laptop never sends "OFF", stop the rotation motor anyway.
const unsigned long MOTOR_MAX_ON_MS = 8000;

// ------------------------------------------------------------------
// Load cell settings
// ------------------------------------------------------------------

// Calibrated on this machine with a 98 g object: with the old factor
// -688.033 the empty platform read +0.4 g and the 98 g object read -21.5 g,
// so the correct factor is -688.033 * (-21.9 / 98) = +153.8 (positive).
// If a known weight reads too low/high, scale this value by
// (shown grams / real grams); if it reads negative, flip the sign.
const float CALIBRATION_FACTOR = 153.8;

const float WEIGHT_PRESENT_THRESHOLD_G = 5.0;   // min grams = "object present"
const float WEIGHT_HEAVY_THRESHOLD_G = 100.0;   // normal heavy vs normal light
const float WEIGHT_STABLE_TOLERANCE_G = 2.0;    // readings this close = settled

// ------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------

HX711 scale;
Servo servo1, servo2, servo3;

WiFiClient espClient;
PubSubClient client(espClient);

enum State { IDLE, WAITING_FOR_RESULT, WAIT_OBJECT_REMOVED };
State state = IDLE;
float currentWeight = 0;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

void relayWrite(int pin, bool activeHigh, bool on) {
  bool level = activeHigh ? on : !on;
  digitalWrite(pin, level ? HIGH : LOW);
}

bool motorRunning = false;
unsigned long motorStartedAt = 0;

void setMotor(bool on) {
  relayWrite(RELAY_MOTOR_PIN, MOTOR_RELAY_ACTIVE_HIGH, on);
  motorRunning = on;
  if (on) motorStartedAt = millis();
}

// Like delay(), but keeps servicing the MQTT client so the connection
// doesn't drop during longer waits (servo moves, conveyor run, etc).
void mqttDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    client.loop();
    delay(20);
  }
}

void setupWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // WiFi power-saving makes the supply current pulse, which adds noise
  // to the HX711 reading. Keep the radio awake for a steadier weight.
  WiFi.setSleep(false);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println(" connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnectMqtt() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT broker...");
    if (client.connect(MQTT_CLIENT_ID)) {
      Serial.println(" connected");
      client.subscribe(TOPIC_RESULT);
      client.subscribe(TOPIC_MOTOR);
    } else {
      Serial.print(" failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 2s");
      delay(2000);
    }
  }
}

// ------------------------------------------------------------------
// Load cell: zeroing, stable reading
// ------------------------------------------------------------------

// Wait until the reading stops changing (the orange has finished
// settling on the platform), so we don't record a half-placed weight.
float readStableWeight() {
  float previous = scale.get_units(5);
  for (int i = 0; i < 10; i++) {
    mqttDelay(300);
    float current = scale.get_units(5);
    if (fabs(current - previous) < WEIGHT_STABLE_TOLERANCE_G) {
      return current;
    }
    previous = current;
  }
  return previous;
}

void printWeightPeriodically(float weight) {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.printf("Weight: %.1f g\n", weight);
  }
}

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd != "tare") {
    Serial.println("Commands: tare (zero the scale)");
    return;
  }
  if (state != IDLE) {
    Serial.println("Busy with an orange, try again when idle.");
    return;
  }

  scale.tare(20);
  Serial.println("Zeroed. Keep the platform empty.");
}

// ------------------------------------------------------------------
// Actions
// ------------------------------------------------------------------

void runServo1() {
  // 90 degrees and back, ~2 seconds total
  servo1.write(90);
  mqttDelay(1000);
  servo1.write(0);
  mqttDelay(1000);
}

void runConveyorCycle(bool defected) {
  relayWrite(RELAY_CONVEYOR_PIN, CONVEYOR_RELAY_ACTIVE_HIGH, true);  // conveyor ON

  if (defected) {
    mqttDelay(5000);
  } else if (currentWeight > WEIGHT_HEAVY_THRESHOLD_G) {
    servo2.write(45);
    mqttDelay(5000);
    servo2.write(0);
  } else {
    servo3.write(45);
    mqttDelay(5000);
    servo3.write(0);
  }

  relayWrite(RELAY_CONVEYOR_PIN, CONVEYOR_RELAY_ACTIVE_HIGH, false);  // conveyor OFF
}

// ------------------------------------------------------------------
// MQTT callback
// ------------------------------------------------------------------

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("[%s] %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_MOTOR) {
    // Only spin while an orange is waiting for inspection.
    setMotor(msg == "ON" && state == WAITING_FOR_RESULT);
    return;
  }

  if (String(topic) != TOPIC_RESULT || state != WAITING_FOR_RESULT) {
    return;
  }

  setMotor(false);  // inspection is over

  bool defected = (msg == "DEFECTED");

  runServo1();
  runConveyorCycle(defected);

  state = WAIT_OBJECT_REMOVED;
}

// ------------------------------------------------------------------
// Setup / loop
// ------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Write the OFF level before switching to OUTPUT so relays don't click on at boot.
  relayWrite(RELAY_CONVEYOR_PIN, CONVEYOR_RELAY_ACTIVE_HIGH, false);
  relayWrite(RELAY_MOTOR_PIN, MOTOR_RELAY_ACTIVE_HIGH, false);
  pinMode(RELAY_CONVEYOR_PIN, OUTPUT);
  pinMode(RELAY_MOTOR_PIN, OUTPUT);
  setMotor(false);

  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo3.attach(SERVO3_PIN);
  servo1.write(0);
  servo2.write(0);
  servo3.write(0);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  while (!scale.wait_ready_timeout(1000)) {
    Serial.println("HX711 not found. Check DOUT/SCK/VCC/GND wiring.");
  }
  scale.set_scale(CALIBRATION_FACTOR);

  setupWifi();
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setCallback(onMqttMessage);
  reconnectMqtt();

  // Zero the scale only now. The HX711 needs a moment to settle after
  // power-up and WiFi start-up shifts its reading, so zeroing earlier
  // leaves a false weight when the platform is empty.
  Serial.println("Zeroing scale, keep the platform empty...");
  mqttDelay(2000);
  scale.tare(20);
  Serial.println("Ready. Type 'tare' to zero the scale.");
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();
  handleSerialCommands();

  if (motorRunning && millis() - motorStartedAt > MOTOR_MAX_ON_MS) {
    Serial.println("Motor timeout, stopping rotation motor.");
    setMotor(false);
  }

  float weight = scale.get_units(5);

  switch (state) {
    case IDLE:
      printWeightPeriodically(weight);
      if (weight > WEIGHT_PRESENT_THRESHOLD_G) {
        float stableWeight = readStableWeight();
        if (stableWeight > WEIGHT_PRESENT_THRESHOLD_G) {
          currentWeight = stableWeight;
          Serial.printf("Object detected, weight = %.1f g\n", currentWeight);
          client.publish(TOPIC_DETECT, "OBJECT_DETECTED");
          state = WAITING_FOR_RESULT;
        }
      }
      break;

    case WAITING_FOR_RESULT:
      // Nothing to do here; onMqttMessage() advances the state
      // once the laptop publishes the DEFECTED/NORMAL result.
      break;

    case WAIT_OBJECT_REMOVED:
      // Wait for the orange to leave the platform before arming
      // detection again, so the same orange isn't counted twice.
      // The platform is empty at this point, so re-zero to cancel drift.
      if (weight < WEIGHT_PRESENT_THRESHOLD_G) {
        scale.tare(10);
        Serial.println("Ready for next orange");
        state = IDLE;
      }
      break;
  }

  delay(100);
}
