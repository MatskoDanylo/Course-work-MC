#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int pirPin = D5;
const int servoPin = D4;

const char* DEVICE_ID = "pochtomat_01";
const char* backend_event_url = "https://your-backend.com/api/mail-event";
const char* backend_command_url = "https://your-backend.com/api/command";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
Servo myServo;

unsigned long lastPirCheck = 0;
unsigned long lastNTPUpdate = 0;
unsigned long lastMotionSentTime = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastScreenUpdate = 0;

const unsigned long pirCheckInterval = 100;
const unsigned long ntpUpdateInterval = 60000;
const unsigned long motionCooldown = 10000;
const unsigned long commandCheckInterval = 5000;
const unsigned long screenUpdateInterval = 2000;

bool motionDetected = false;
bool doorIsOpen = false;

int currentScreenMode = 0; // 0 = time/date, 1 = last mail
String lastMailTime = "N/A";

void printOLED(String line1, String line2 = "", String line3 = "") {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.println(line1);
  if (line2 != "") display.println(line2);
  if (line3 != "") display.println(line3);
  display.display();
}

String getFormattedDateTime() {
  time_t rawTime = timeClient.getEpochTime();
  struct tm* ti = localtime(&rawTime);
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
           ti->tm_hour, ti->tm_min, ti->tm_sec);
  return String(buf);
}

void sendEvent(String eventType) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, backend_event_url);
    http.addHeader("Content-Type", "application/json");

    String datetime = getFormattedDateTime();

    StaticJsonDocument<256> jsonDoc;
    jsonDoc["device_id"] = DEVICE_ID;
    jsonDoc["event"] = eventType;
    jsonDoc["time"] = datetime;

    String requestBody;
    serializeJson(jsonDoc, requestBody);

    int httpResponseCode = http.POST(requestBody);
    Serial.println("HTTP: " + String(httpResponseCode));

    if (eventType == "new_mail") {
      lastMailTime = datetime;
    }

    http.end();
  }
}

void openDoor() {
  myServo.write(90);
  doorIsOpen = true;
  sendEvent("door_opened");
}

void closeDoor() {
  myServo.write(0);
  doorIsOpen = false;
  sendEvent("door_closed");
}

void checkCommandFromBackend() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    String fullUrl = String(backend_command_url) + "?device_id=" + DEVICE_ID;
    http.begin(client, fullUrl);
    int httpCode = http.GET();

    if (httpCode == 200) {
      String response = http.getString();
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        if (doc.containsKey("command")) {
          String command = doc["command"];
          if (command == "open" && !doorIsOpen) openDoor();
          else if (command == "close" && doorIsOpen) closeDoor();
        }

        if (doc.containsKey("screen_mode")) {
          currentScreenMode = doc["screen_mode"];
        }
      }
    }

    http.end();
  }
}

void updateDisplay() {
  if (currentScreenMode == 0) {
    printOLED("Time/Date:", getFormattedDateTime());
  } else if (currentScreenMode == 1) {
    printOLED("Last mail:", lastMailTime);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(pirPin, INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    sendEvent("error");
    while (true);
  }

  printOLED("Pochtomat booting...");

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect("Pochtomat-Setup")) {
    printOLED("WiFi error", "Rebooting...");
    sendEvent("error");
    delay(1000);
    ESP.restart();
  }

  timeClient.begin();
  timeClient.update();
  lastNTPUpdate = millis();

  myServo.attach(servoPin);
  myServo.write(0);
  doorIsOpen = false;

  sendEvent("rebooted");
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastNTPUpdate >= ntpUpdateInterval) {
    timeClient.update();
    lastNTPUpdate = currentMillis;
  }

  if (currentMillis - lastPirCheck >= pirCheckInterval) {
    lastPirCheck = currentMillis;

    int pirState = digitalRead(pirPin);
    if (pirState == HIGH && !motionDetected) {
      motionDetected = true;

      if (currentMillis - lastMotionSentTime >= motionCooldown) {
        sendEvent("new_mail");
        lastMotionSentTime = currentMillis;
      }
    } else if (pirState == LOW && motionDetected) {
      motionDetected = false;
    }
  }

  if (currentMillis - lastCommandCheck >= commandCheckInterval) {
    lastCommandCheck = currentMillis;
    checkCommandFromBackend();
  }

  if (currentMillis - lastScreenUpdate >= screenUpdateInterval) {
    lastScreenUpdate = currentMillis;
    updateDisplay();
  }
}
