/*
  Orange Defect Detection - ESP32 application.

  - Reads a load cell (HX711) to detect when an orange has been placed
    on the platform, and tells the Raspberry Pi over MQTT.
  - Waits for the Raspberry Pi to report "DEFECTED" or "NORMAL".
  - Moves servo1 (drop gate) 90 degrees and back, then turns on the
    conveyor relay and routes the orange using servo2 (normal + heavy)
    or servo3 (normal + light), or just the conveyor alone if defected.
  - Runs forever, one orange at a time.

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
// Load cell calibration
// ------------------------------------------------------------------

// TODO: calibrate for your specific load cell + HX711 module.
const float LOADCELL_CALIBRATION_FACTOR = 2280.0;
const float WEIGHT_PRESENT_THRESHOLD_G = 5.0;   // min grams = "object present"
const float WEIGHT_HEAVY_THRESHOLD_G = 100.0;   // normal heavy vs normal light

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
  scale.set_scale(LOADCELL_CALIBRATION_FACTOR);
  scale.tare();

  setupWifi();
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setCallback(onMqttMessage);
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();

  float weight = scale.get_units(5);
  if (weight < 0) weight = 0;

  switch (state) {
    case IDLE:
      if (weight > WEIGHT_PRESENT_THRESHOLD_G) {
        currentWeight = weight;
        Serial.printf("Object detected, weight = %.1f g\n", currentWeight);
        client.publish(TOPIC_DETECT, "OBJECT_DETECTED");
        state = WAITING_FOR_RESULT;
      }
      break;

    case WAITING_FOR_RESULT:
      // Nothing to do here; onMqttMessage() advances the state
      // once the Pi publishes the DEFECTED/NORMAL result.
      break;

    case WAIT_OBJECT_REMOVED:
      // Debounce: wait for the orange to leave the platform before
      // arming detection again, so the same orange isn't counted twice.
      if (weight < WEIGHT_PRESENT_THRESHOLD_G) {
        Serial.println("Ready for next orange");
        state = IDLE;
      }
      break;
  }

  delay(100);
}
