#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>

// --- Pinout Configuration ---
#define RELAY_PIN   26
#define SENSOR_PIN  25

// --- WiFi & MQTT Configuration ---
const char* ssid = "2024";
const char* password = "2024@uwc";
const char* mqtt_server = "172.17.132.63";
const int   mqtt_port = 1883;
const char* mqtt_data_topic = "grp6_irrigation_data";
const char* mqtt_command_topic = "grp6_irrigation_command";

// --- Time (NTP) Configuration ---
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 7200;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

// --- Global Variables ---
volatile byte pulseCount;
float calibrationFactor = 85.0;
float flowRate;
unsigned long totalMilliLitres = 0;   // **NEW: Tracks total volume, never resets**
unsigned long cycleMilliLitres = 0;   // **NEW: Tracks volume for the current cycle**
bool pumpState = false;

// Timers
unsigned long previousMillis = 0;
unsigned long minuteMillis = 0;
unsigned long pumpStartTime = 0;
const unsigned long FAILSAFE_DURATION = 3600000UL; // 1 hour
int interval = 1000;

// Minute Average
float totalFlowRateForMinute = 0;
int readingCountForMinute = 0;

WiFiClient espClient;
PubSubClient client(espClient);

// --- Function to send the final OFF status ---
void sendPumpOffStatus() {
  char formattedTime[20];
  if (timeClient.getEpochTime() < 1609459200) {
      strcpy(formattedTime, "Syncing...");
  } else {
      time_t epochTime = timeClient.getEpochTime();
      struct tm* timeinfo = localtime(&epochTime);
      strftime(formattedTime, sizeof(formattedTime), "%Y-%m-%d %H:%M:%S", timeinfo);
  }

  StaticJsonDocument<300> doc;
  doc["timestamp"] = formattedTime;
  doc["flow_rate_Lmin"] = 0.00;
  doc["total_volume"] = float_with_three_decimals(totalMilliLitres / 1000.0);
  doc["water_used_cycle"] = float_with_three_decimals(cycleMilliLitres / 1000.0);
  doc["pump_state"] = "OFF";
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);
  client.publish(mqtt_data_topic, jsonBuffer);

  Serial.println("\n---------------------");
  Serial.println("PUMP TURNED OFF");
  Serial.print("Final Water (Cycle): ");
  Serial.print(cycleMilliLitres / 1000.0, 3);
  Serial.println(" L");
  Serial.print("Total Water Used:    ");
  Serial.print(totalMilliLitres / 1000.0, 3);
  Serial.println(" L");
  Serial.println("---------------------\n");
}

// --- Function to handle incoming MQTT commands ---
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Command received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (String(topic) == mqtt_command_topic) {
    if (message == "ON" && !pumpState) {
      digitalWrite(RELAY_PIN, LOW);
      pumpState = true;
      pumpStartTime = millis();
      cycleMilliLitres = 0; // **KEY CHANGE: Reset cycle volume on ON**
    } else if (message == "OFF" && pumpState) {
      digitalWrite(RELAY_PIN, HIGH);
      pumpState = false;
      sendPumpOffStatus();
    }
  }
}

void IRAM_ATTR pulseCounter() {
  if (pumpState) {
    pulseCount++;
  }
}

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32-Irrigation-Client")) {
      Serial.println("connected");
      client.subscribe(mqtt_command_topic);
      Serial.print("Subscribed to command topic: ");
      Serial.println(mqtt_command_topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, HIGH);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  timeClient.begin();
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), pulseCounter, FALLING);
  Serial.println("\nIrrigation System Monitor");
  Serial.println("Pump is OFF. Waiting for command...\n");
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  timeClient.update();

  unsigned long currentMillis = millis();

  // Failsafe Check
  if (pumpState && (currentMillis - pumpStartTime >= FAILSAFE_DURATION)) {
    Serial.println("\n*****************************************");
    Serial.println("!!! FAILSAFE TRIGGERED !!!");
    
    digitalWrite(RELAY_PIN, HIGH);
    pumpState = false;
    sendPumpOffStatus();
  }

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    if (pumpState) {
      detachInterrupt(digitalPinToInterrupt(SENSOR_PIN));
      byte pulse1Sec = pulseCount;
      pulseCount = 0;
      attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), pulseCounter, FALLING);

      flowRate = ((1000.0 / interval) * pulse1Sec) / calibrationFactor;
      unsigned long flowMilliLitres = (flowRate / 60) * 1000;
      
      // **KEY CHANGE: Add water to both totals**
      totalMilliLitres += flowMilliLitres;
      cycleMilliLitres += flowMilliLitres;

      totalFlowRateForMinute += flowRate;
      readingCountForMinute++;

      char formattedTime[20];
      if (timeClient.getEpochTime() < 1609459200) {
          strcpy(formattedTime, "Syncing...");
      } else {
          time_t epochTime = timeClient.getEpochTime();
          struct tm* timeinfo = localtime(&epochTime);
          strftime(formattedTime, sizeof(formattedTime), "%Y-%m-%d %H:%M:%S", timeinfo);
      }

      StaticJsonDocument<300> doc;
      doc["timestamp"] = formattedTime;
      doc["flow_rate_Lmin"] = float_with_two_decimals(flowRate);
      doc["total_volume"] = float_with_three_decimals(totalMilliLitres / 1000.0);
      doc["water_used_cycle"] = float_with_three_decimals(cycleMilliLitres / 1000.0);
      doc["pump_state"] = "ON";
      char jsonBuffer[512];
      serializeJson(doc, jsonBuffer);
      client.publish(mqtt_data_topic, jsonBuffer);

      Serial.println("--- Sensor Update ---");
      Serial.print("Timestamp:         "); Serial.println(formattedTime);
      Serial.print("Flow Rate:         "); Serial.print(flowRate, 2); Serial.println(" L/min");
      Serial.print("Water Used (Cycle): "); Serial.print(cycleMilliLitres / 1000.0, 3); Serial.println(" L");
      Serial.print("Total Volume:      "); Serial.print(totalMilliLitres / 1000.0, 3); Serial.println(" L");
      Serial.print("Pump State:        ON\n");
      Serial.println("---------------------");
    }
  }

  if (pumpState && (currentMillis - minuteMillis >= 60000)) {
    minuteMillis = currentMillis;
    if (readingCountForMinute > 0) {
      float averageFlowRate = totalFlowRateForMinute / readingCountForMinute;
      Serial.println("*****************************************");
      Serial.println("           MINUTE SUMMARY");
      Serial.print("Average Flow Rate: ");
      Serial.print(averageFlowRate, 2);
      Serial.println(" L/min");
      Serial.println("*****************************************\n");
      totalFlowRateForMinute = 0;
      readingCountForMinute = 0;
    }
  }
}

float float_with_two_decimals(float value) {
  return (int)(value * 100) / 100.0;
}
float float_with_three_decimals(float value) {
  return (int)(value * 1000) / 1000.0;
}