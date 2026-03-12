#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <HTTPClient.h>
#include <time.h>

// ===== WiFi =====
const char *ssid = "iPhone";
const char *password = "avocado1";

WiFiClient wifiClient;

// ===== NETPIE =====
const char *mqttServer = "mqtt.netpie.io";
const int mqttPort = 1883;
const char *mqttClientId = "ba136572-526c-413f-aa9e-59b5ac140196";
const char *mqttUser = "4LTrPF8abS7fY6GwmNQNdvpeF7unQum8";
const char *mqttPassword = "ph9H6DXvNz2yUP2qpcMobzXGJzC55DcZ";

const char *shadow_pub = "@shadow/data/update";
const char *msg_pub = "@msg/parking";
const char *control_sub = "@msg/parking/control";

PubSubClient mqttClient(wifiClient);

// ===== Firebase =====
#define DATABASE_URL "https://carparking-982c9-default-rtdb.asia-southeast1.firebasedatabase.app/parking.json"

// ===== TFT Display (ILI9341 SPI) =====
#define TFT_CS  33
#define TFT_RST  4
#define TFT_DC   25

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// ===== IR Sensors =====
#define IR1 35
#define IR2 36
#define IR3 34
#define IR4 26

const int irPins[] = {IR1, IR2, IR3, IR4};
const int NUM_SLOTS = 4;

int prevStatus[NUM_SLOTS] = {-1, -1, -1, -1};
time_t entryTime[NUM_SLOTS] = {0, 0, 0, 0};

unsigned long lastShadowUpdate = 0;
const unsigned long shadowInterval = 2000;

bool sensorEnabled = true;

// ===== Function Prototypes =====
String getTimeString(time_t t);
String getDateTimeString(time_t t);
String getDurationString(time_t start, time_t now);

void setup_wifi();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishShadow();
void logParkingHistoryToFirebase(int slot, time_t entryTimeValue);
void sendToFirebase();
void updateDisplay();

// -------------------- Time Helpers --------------------
String getTimeString(time_t t) {
  struct tm* ti = localtime(&t);
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", ti);
  return String(buf);
}

String getDateTimeString(time_t t) {
  struct tm* ti = localtime(&t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);
  return String(buf);
}

String getDurationString(time_t start, time_t now) {
  long diff = (long)difftime(now, start);
  int h = diff / 3600;
  int m = (diff % 3600) / 60;
  int s = diff % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

// -------------------- WiFi --------------------
void setup_wifi() {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  if (strlen(password) == 0) {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, password);
  }

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    retry++;
    if (retry > 40) {
      Serial.println("\nWiFi connect failed! restart...");
      ESP.restart();
    }
  }

  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());
}

// -------------------- MQTT Callback --------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(msg);

  if (String(topic) == control_sub) {
    if (msg.indexOf("\"sensor\":\"on\"") >= 0) {
      sensorEnabled = true;
      Serial.println("Sensor ENABLED");
      publishShadow();
      updateDisplay();
      sendToFirebase();
    } else if (msg.indexOf("\"sensor\":\"off\"") >= 0) {
      sensorEnabled = false;
      Serial.println("Sensor DISABLED");
      publishShadow();
      updateDisplay();
      sendToFirebase();
    }
  }
}

// -------------------- MQTT Reconnect --------------------
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.println("Connecting to NETPIE...");
    if (mqttClient.connect(mqttClientId, mqttUser, mqttPassword)) {
      Serial.println("MQTT connected");
      mqttClient.subscribe(control_sub);
    } else {
      Serial.print("failed rc=");
      Serial.println(mqttClient.state());
      delay(5000);
    }
  }
}

// -------------------- Publish MQTT --------------------
void publishShadow() {
  int ir[NUM_SLOTS];

  for (int i = 0; i < NUM_SLOTS; i++) {
    if (sensorEnabled) {
      ir[i] = (digitalRead(irPins[i]) == LOW) ? 1 : 0;
    } else {
      ir[i] = 0;
    }
  }

  String msg = "{ \"data\": {";
  msg += "\"ir1\":" + String(ir[0]) + ",";
  msg += "\"ir2\":" + String(ir[1]) + ",";
  msg += "\"ir3\":" + String(ir[2]) + ",";
  msg += "\"ir4\":" + String(ir[3]) + ",";
  msg += "\"sensor_enabled\":" + String(sensorEnabled ? 1 : 0);
  msg += "} }";

  Serial.println("Publish: " + msg);
  mqttClient.publish(shadow_pub, msg.c_str());
  mqttClient.publish(msg_pub, msg.c_str());
}

// -------------------- Firebase History --------------------
void logParkingHistoryToFirebase(int slot, time_t entryTimeValue) {
  HTTPClient http;
  String url = "https://carparking-982c9-default-rtdb.asia-southeast1.firebasedatabase.app/history.json";

  String payload = "{";
  payload += "\"slot\":" + String(slot + 1) + ",";
  payload += "\"entry_time\":\"" + getDateTimeString(entryTimeValue) + "\",";
  payload += "\"timestamp\":\"" + getDateTimeString(time(nullptr)) + "\"";
  payload += "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    Serial.println("History logged: " + http.getString());
  } else {
    Serial.println("Error logging history. Code: " + String(httpResponseCode));
  }

  http.end();
}

// -------------------- Firebase Current Status --------------------
void sendToFirebase() {
  HTTPClient http;
  String url = DATABASE_URL;

  int ir[NUM_SLOTS];
  int occupied = 0;

  for (int i = 0; i < NUM_SLOTS; i++) {
    if (sensorEnabled) {
      ir[i] = (digitalRead(irPins[i]) == LOW) ? 1 : 0;
    } else {
      ir[i] = 0;
    }

    if (ir[i]) occupied++;
  }

  int available = NUM_SLOTS - occupied;
  time_t now = time(nullptr);

  String payload = "{";
  for (int i = 0; i < NUM_SLOTS; i++) {
    payload += "\"slot" + String(i + 1) + "\": {";
    payload += "\"status\": " + String(ir[i]) + ",";

    if (ir[i] && entryTime[i] > 0 && sensorEnabled) {
      payload += "\"entry_time\": \"" + getDateTimeString(entryTime[i]) + "\",";
      payload += "\"duration\": \"" + getDurationString(entryTime[i], now) + "\"";
    } else {
      payload += "\"entry_time\": \"\",";
      payload += "\"duration\": \"\"";
    }

    payload += "},";
  }

  payload += "\"summary\": {";
  payload += "\"occupied\": " + String(occupied) + ",";
  payload += "\"available\": " + String(available) + ",";
  payload += "\"total\": " + String(NUM_SLOTS) + ",";
  payload += "\"sensor_enabled\": " + String(sensorEnabled ? 1 : 0) + ",";
  payload += "\"updated\": \"" + getDateTimeString(now) + "\"";
  payload += "}}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.PUT(payload);

  if (httpResponseCode > 0) {
    Serial.println("Firebase response: " + http.getString());
  } else {
    Serial.println("Error sending to Firebase. Code: " + String(httpResponseCode));
  }

  http.end();
}

// -------------------- TFT Display --------------------
void updateDisplay() {
  if (!sensorEnabled) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(70, 20);
    tft.println("CAR PARKING");

    tft.drawLine(10, 50, 310, 50, ILI9341_WHITE);

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(35, 100);
    tft.println("SENSOR DISABLED");
    return;
  }

  int occupied = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (digitalRead(irPins[i]) == LOW) {
      occupied++;
    }
  }
  int available = NUM_SLOTS - occupied;

  tft.fillScreen(ILI9341_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(70, 5);
  tft.println("CAR PARKING");

  tft.drawLine(10, 28, 310, 28, ILI9341_WHITE);

  tft.setCursor(20, 38);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("Occupied: ");
  tft.setTextColor(ILI9341_RED);
  tft.println(String(occupied) + "/" + String(NUM_SLOTS));

  tft.setCursor(20, 60);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("Available: ");
  tft.setTextColor(ILI9341_GREEN);
  tft.println(available);

  tft.drawLine(10, 85, 310, 85, ILI9341_WHITE);

  time_t now = time(nullptr);
  for (int i = 0; i < NUM_SLOTS; i++) {
    int y = 95 + i * 40;
    int det = (digitalRead(irPins[i]) == LOW) ? 1 : 0;

    tft.setCursor(20, y);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.print("S" + String(i + 1) + ":");

    if (det) {
      tft.fillRect(80, y - 2, 50, 20, ILI9341_RED);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(83, y);
      tft.print("FULL");

      if (entryTime[i] > 0) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(140, y);
        tft.print(getDurationString(entryTime[i], now));
      }
    } else {
      tft.fillRect(80, y - 2, 50, 20, ILI9341_GREEN);
      tft.setTextColor(ILI9341_BLACK);
      tft.setCursor(83, y);
      tft.print("FREE");
    }
  }
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(irPins[i], INPUT);
  }

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(30, 100);
  tft.println("Starting...");

  setup_wifi();

  configTime(7 * 3600, 0, "pool.ntp.org");
  Serial.println("Waiting for NTP time sync...");
  while (time(nullptr) < 100000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced!");

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();

  Serial.println("Parking System Started");

  for (int i = 0; i < NUM_SLOTS; i++) {
    prevStatus[i] = digitalRead(irPins[i]);
  }

  publishShadow();
  updateDisplay();
  sendToFirebase();
}

// -------------------- Loop --------------------
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  if (!sensorEnabled) {
    unsigned long now = millis();
    if (now - lastShadowUpdate >= shadowInterval) {
      publishShadow();
      updateDisplay();
      sendToFirebase();
      lastShadowUpdate = now;
    }
    delay(200);
    return;
  }

  bool changed = false;

  for (int i = 0; i < NUM_SLOTS; i++) {
    int current = digitalRead(irPins[i]);

    if (current != prevStatus[i]) {
      int detected = (current == LOW) ? 1 : 0;
      Serial.println("ir " + String(i + 1) + " = " + String(detected));

      if (detected == 1 && prevStatus[i] != LOW) {
        entryTime[i] = time(nullptr);
        Serial.println("Slot " + String(i + 1) + " entry at: " + getDateTimeString(entryTime[i]));
        logParkingHistoryToFirebase(i, entryTime[i]);
      } else if (detected == 0 && prevStatus[i] == LOW) {
        if (entryTime[i] > 0) {
          String duration = getDurationString(entryTime[i], time(nullptr));
          Serial.println("Slot " + String(i + 1) + " exit. Duration: " + duration);
        }
        entryTime[i] = 0;
      }

      prevStatus[i] = current;
      changed = true;
    }
  }

  unsigned long now = millis();
  if (changed || (now - lastShadowUpdate >= shadowInterval)) {
    publishShadow();
    updateDisplay();
    sendToFirebase();
    lastShadowUpdate = now;
  }

  delay(200);
}