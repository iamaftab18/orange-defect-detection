/*
  Orange Defect Detection - ESP32 application.

  - Reads a load cell (HX711) to detect when an orange has been placed
    on the platform, and tells the Raspberry Pi over MQTT.
  - Waits for the Raspberry Pi to report "DEFECTED" or "NORMAL".
  - Moves servo1 (drop gate) 90 degrees and back, then turns on the
    conveyor relay and routes the orange using servo2 (normal + heavy)
    or servo3 (normal + light), or just the conveyor alone if defected.
  - Runs forever, one orange at a time.

  Serial Monitor (115200 baud) commands:
    cal   - guided load cell calibration (result is saved in flash)
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
#include <Preferences.h>

// ------------------------------------------------------------------
// WiFi / MQTT configuration
// ------------------------------------------------------------------

const char *WIFI_SSID = "network";
const char *WIFI_PASSWORD = "1122334455";

const char *MQTT_BROKER = "broker.hivemq.com";  // HiveMQ free public broker
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "esp32-orange-sorter";

const char *TOPIC_DETECT = "orangesort/detect";  // ESP32 -> Pi
const char *TOPIC_RESULT = "orangesort/result";  // Pi -> ESP32

// ------------------------------------------------------------------
// Pin configuration
// ------------------------------------------------------------------

const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN = 4;

const int RELAY_CONVEYOR_PIN = 17;  // conveyor belt motor relay
const int SERVO1_PIN = 18;          // drop gate, right after rotation
const int SERVO2_PIN = 19;          // normal + heavy (>100g) path
const int SERVO3_PIN = 21;          // normal + light (<=100g) path

const bool RELAY_ACTIVE_HIGH = true;  // set false if your relay turns ON on LOW

// ------------------------------------------------------------------
// Load cell settings
// ------------------------------------------------------------------

// Only used until you run the "cal" command once; after that the value
// saved in flash is used instead.
const float DEFAULT_CALIBRATION_FACTOR = 2280.0;

const float WEIGHT_PRESENT_THRESHOLD_G = 5.0;   // min grams = "object present"
const float WEIGHT_HEAVY_THRESHOLD_G = 100.0;   // normal heavy vs normal light
const float WEIGHT_STABLE_TOLERANCE_G = 2.0;    // readings this close = settled

// ------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------

HX711 scale;
Preferences prefs;
Servo servo1, servo2, servo3;

WiFiClient espClient;
PubSubClient client(espClient);

enum State { IDLE, WAITING_FOR_RESULT, WAIT_OBJECT_REMOVED };
State state = IDLE;
float currentWeight = 0;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

void relayWrite(int pin, bool on) {
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(pin, level ? HIGH : LOW);
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
    } else {
      Serial.print(" failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 2s");
      delay(2000);
    }
  }
}

// ------------------------------------------------------------------
// Load cell: calibration, zeroing, stable reading
// ------------------------------------------------------------------

// Applies the calibration factor saved in flash (or the default).
void loadCalibration() {
  prefs.begin("scale", false);
  float factor = prefs.getFloat("factor", 0.0);
  prefs.end();

  if (factor != 0.0) {
    scale.set_scale(factor);
    Serial.printf("Loaded saved calibration factor: %.2f\n", factor);
  } else {
    scale.set_scale(DEFAULT_CALIBRATION_FACTOR);
    Serial.println("No saved calibration yet, using default. Type 'cal' to calibrate.");
  }
}

void saveCalibration(float factor) {
  prefs.begin("scale", false);
  prefs.putFloat("factor", factor);
  prefs.end();
}

// Waits for a line typed in the Serial Monitor, keeping MQTT alive.
String readLine() {
  while (!Serial.available()) {
    client.loop();
    delay(20);
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  return line;
}

void calibrate() {
  Serial.println();
  Serial.println("=== LOAD CELL CALIBRATION ===");
  Serial.println("(Serial Monitor line ending must be set to 'Newline')");
  Serial.println("Step 1: remove everything from the platform, then press Enter.");
  readLine();

  scale.set_scale(1.0);
  scale.tare(20);
  Serial.println("Zero set.");

  Serial.println("Step 2: place a known weight on the platform and wait a few seconds.");
  Serial.println("        Then type its weight in grams (e.g. 100) and press Enter.");
  float knownGrams = readLine().toFloat();
  if (knownGrams <= 0) {
    Serial.println("Invalid weight, calibration cancelled.");
    loadCalibration();
    return;
  }

  Serial.println("Measuring...");
  float rawChange = scale.get_value(20);  // change from zero, before scaling
  if (fabs(rawChange) < 100) {
    Serial.println("Almost no change detected. Is the weight on the platform and the");
    Serial.println("load cell wired correctly? Calibration cancelled.");
    loadCalibration();
    return;
  }

  float factor = rawChange / knownGrams;
  scale.set_scale(factor);
  saveCalibration(factor);
  Serial.printf("Calibration factor %.2f saved to flash.\n", factor);

  Serial.println("Check: the weight now reads");
  for (int i = 0; i < 3; i++) {
    Serial.printf("  %.1f g\n", scale.get_units(5));
  }

  Serial.println("Step 3: remove the weight from the platform, then press Enter.");
  readLine();
  scale.tare(20);
  Serial.println("Calibration done. Normal operation resumes.");
}

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

  if (cmd != "cal" && cmd != "tare") {
    Serial.println("Commands: cal (calibrate), tare (zero the scale)");
    return;
  }
  if (state != IDLE) {
    Serial.println("Busy with an orange, try again when idle.");
    return;
  }

  if (cmd == "cal") {
    calibrate();
  } else {
    scale.tare(20);
    Serial.println("Zeroed. Keep the platform empty.");
  }
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
  relayWrite(RELAY_CONVEYOR_PIN, true);  // conveyor ON

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

  relayWrite(RELAY_CONVEYOR_PIN, false);  // conveyor OFF
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

  if (String(topic) != TOPIC_RESULT || state != WAITING_FOR_RESULT) {
    return;
  }

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

  pinMode(RELAY_CONVEYOR_PIN, OUTPUT);
  relayWrite(RELAY_CONVEYOR_PIN, false);

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
  loadCalibration();

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
  Serial.println("Ready. Type 'cal' to calibrate or 'tare' to zero the scale.");
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();
  handleSerialCommands();

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
      // once the Pi publishes the DEFECTED/NORMAL result.
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
