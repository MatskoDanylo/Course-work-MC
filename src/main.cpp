#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoWebsockets.h>
#include <EEPROM.h>
#include <LittleFS.h>  // Add LittleFS support

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SECRET_WORD "7c2be9b808fc408a47cd130d8254f65dd9a7f76041a7b8e45ceff001b56fce97"

// Constants for LittleFS
#define DEVICE_ID_FILE "/device_id.txt"

// EEPROM settings
#define EEPROM_SIZE 16
#define DEVICE_ID_ADDR 0
#define EEPROM_INITIALIZED_ADDR 4
#define EEPROM_INITIALIZED_VALUE 123

using namespace websockets;

WebsocketsClient wsClient;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int pirPin = D5;
const int servoPin = D4;

long DEVICE_ID = -1;

// Server URLs with string concatenation
String currentServerIP = "192.168.1.103";
bool usingFallbackServer = false;

// Dynamic URL construction
String backend_event_url;
String backend_command_url;
String backend_register_url;
String backend_gateway_url;

// Function to update all URLs based on current server IP
void updateServerURLs() {
  backend_event_url = "http://" + currentServerIP + ":4000/microcontroller/event";
  backend_command_url = "http://" + currentServerIP + ":4000/microcontroller/command";
  backend_register_url = "http://" + currentServerIP + ":4000/microcontroller/register";
  backend_gateway_url = "ws://" + currentServerIP + ":4000";

  Serial.println("Using server IP: " + currentServerIP);
  Serial.println("WebSocket URL: " + backend_gateway_url);
}

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
Servo myServo;

unsigned long lastPirCheck = 0;
unsigned long lastNTPUpdate = 0;
unsigned long lastMotionSentTime = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastScreenUpdate = 0;
unsigned long lastRegisterTime = 0;
unsigned long lastWebSocketRegistrationAttempt = 0;

const unsigned long pirCheckInterval = 100;
const unsigned long ntpUpdateInterval = 60000;
const unsigned long motionCooldown = 10000;
const unsigned long commandCheckInterval = 5000;
const unsigned long screenUpdateInterval = 2000;
const unsigned long registerInterval = 1000; // Try to register every second
const unsigned long wsRegistrationInterval = 1000; // Try WS registration every second if not registered

bool motionDetected = false;
short doorIsOpen = 0;

int currentScreenMode = 0;
String lastMailTime = "N/A";

bool wsConnected = false;
unsigned long lastWsReconnectAttempt = 0;
const unsigned long wsReconnectInterval = 5000;
unsigned long lastPingTime = 0;
const unsigned long pingInterval = 30000;
unsigned long lastPongTime = 0;
int reconnectAttempts = 0;
const int maxReconnectAttempts = 5;
bool registrationSent = false;

// EEPROM management functions
void saveDeviceIdToEEPROM() {
  EEPROM.put(DEVICE_ID_ADDR, DEVICE_ID);
  EEPROM.put(EEPROM_INITIALIZED_ADDR, EEPROM_INITIALIZED_VALUE);
  EEPROM.commit();
  Serial.print("Device ID saved to EEPROM: ");
  Serial.println(DEVICE_ID);
}

bool loadDeviceIdFromEEPROM() {
  int initValue;
  EEPROM.get(EEPROM_INITIALIZED_ADDR, initValue);

  if (initValue == EEPROM_INITIALIZED_VALUE) {
    EEPROM.get(DEVICE_ID_ADDR, DEVICE_ID);
    Serial.print("Loaded Device ID from EEPROM: ");
    Serial.println(DEVICE_ID);
    return true;
  }

  Serial.println("No saved Device ID found in EEPROM");
  return false;
}

// LittleFS management functions
void saveDeviceIdToLittleFS() {
  File file = LittleFS.open(DEVICE_ID_FILE, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.println(DEVICE_ID);
  file.close();
  Serial.print("Device ID saved to LittleFS: ");
  Serial.println(DEVICE_ID);
}

bool loadDeviceIdFromLittleFS() {
  File file = LittleFS.open(DEVICE_ID_FILE, "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return false;
  }
  String idStr = file.readStringUntil('\n');
  DEVICE_ID = idStr.toInt();
  file.close();
  Serial.print("Loaded Device ID from LittleFS: ");
  Serial.println(DEVICE_ID);
  return true;
}

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
  tm* ti = localtime(&rawTime);
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
           ti->tm_hour, ti->tm_min, ti->tm_sec);
  return String(buf);
}

void sendEvent(String eventType) {
  String datetime = getFormattedDateTime();
  StaticJsonDocument<256> jsonDoc;
  jsonDoc["device_id"] = DEVICE_ID;
  jsonDoc["event"] = eventType;
  jsonDoc["time"] = datetime;

  String requestBody;
  serializeJson(jsonDoc, requestBody);

  if (wsConnected) {
    wsClient.send(requestBody);
    Serial.println("WebSocket Event Sent: " + requestBody);
  } else {
    Serial.println("WebSocket not connected, event not sent: " + requestBody);
  }

  if (eventType == "new_mail") {
    lastMailTime = datetime;
  }
}

void openDoor() {
  myServo.write(180);
  doorIsOpen = 1;
  sendEvent("door_opened");
}

void closeDoor() {
  myServo.write(0);
  doorIsOpen = 0;
  sendEvent("door_closed");
}

// Send registration to WebSocket if we have a device ID
void registerWithWebSocket() {
  if (DEVICE_ID != -1 && wsConnected && !registrationSent) {
    String regMsg = "{\"type\":\"register\",\"device_id\":" + String(DEVICE_ID) + "}";
    wsClient.send(regMsg);
    Serial.println("Sent WebSocket registration: " + regMsg);
    registrationSent = true;
    printOLED("Registered", "Device ID: " + String(DEVICE_ID), "WebSocket OK");
  }
}

void tryRegisterDevice() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot register device");
    printOLED("Registration Error", "WiFi not connected");
    return;
  }

  Serial.println("Attempting to register device with backend...");
  printOLED("Registering...", "Connecting to server");

  WiFiClient client;
  HTTPClient http;

  // Set timeout to prevent hanging
  http.setTimeout(5000);

  // Try to begin connection
  bool beginSuccess = http.begin(client, backend_register_url);
  if (!beginSuccess) {
    Serial.println("Failed to connect to registration endpoint");
    printOLED("Reg Error", "Can't reach server", backend_register_url);
    return;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> jsonDoc;
  jsonDoc["secretWord"] = SECRET_WORD;

  String requestBody;
  serializeJson(jsonDoc, requestBody);

  Serial.println("Sending registration request...");
  int httpResponseCode = http.POST(requestBody);
  String response = http.getString();

  Serial.print("Registration attempt response code: ");
  Serial.println(httpResponseCode);
  Serial.print("Response: ");
  Serial.println(response);

  if (httpResponseCode == 201) {
    DEVICE_ID = response.toInt();
    Serial.print("Device registered with ID: ");
    Serial.println(DEVICE_ID);
    printOLED("Registration OK", "Device ID: " + String(DEVICE_ID));

    // Save the device ID to EEPROM for persistence
    saveDeviceIdToEEPROM();
    saveDeviceIdToLittleFS();

    // If we're already connected to WebSocket, send registration immediately
    if (wsConnected) {
      registerWithWebSocket();
    }
  } else {
    Serial.println("Failed to register device with backend");
    printOLED("Reg Failed", "HTTP code: " + String(httpResponseCode), response.substring(0, 20));
  }
  http.end();
}

void updateDisplay() {
  if (currentScreenMode == 0) {
    String statusLine = "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "NC");
    statusLine += " WS: " + String(wsConnected ? "OK" : "NC");
    statusLine += " ID: " + String(DEVICE_ID);
    printOLED("Time/Date:", getFormattedDateTime(), statusLine);
  } else if (currentScreenMode == 1) {
    printOLED("Last mail:", lastMailTime);
  }
}

void onWebSocketMessage(WebsocketsMessage message) {
  Serial.println(message.data());
  String rawMessage = message.data();
  if (rawMessage.startsWith("\"") && rawMessage.endsWith("\"")) {
    rawMessage = rawMessage.substring(1, rawMessage.length() - 1);
  }
  if (rawMessage == "open" && !doorIsOpen) {
    openDoor();
  }
  else if (rawMessage == "close" && doorIsOpen) {
    closeDoor();
  }
}

void onWebSocketEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("WebSocket Connected");
    wsConnected = true;
    reconnectAttempts = 0;
    lastPongTime = millis(); // Reset pong timer on connection

    // Always send registration on successful connection if we have an ID
    registerWithWebSocket();
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("WebSocket Disconnected");
    wsConnected = false;
    registrationSent = false; // Reset registration flag to ensure we register again on reconnection
    lastWsReconnectAttempt = millis() - wsReconnectInterval + 1000; // Try reconnecting soon (after 1 second)
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println("WebSocket Ping received");
    wsClient.pong(); // Respond to ping with pong
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.println("WebSocket Pong received");
    lastPongTime = millis();
  }
}

bool connectWebSocket() {
  Serial.println("Attempting to connect to WebSocket server...");
  bool connected = wsClient.connect(backend_gateway_url);
  Serial.println("WebSocket connection result: " + String(connected ? "Connected!" : "Failed!"));
  return connected;
}

void setup() {
  Serial.begin(115200);
  pinMode(pirPin, INPUT);

  // Initialize EEPROM with enough space
  EEPROM.begin(EEPROM_SIZE);
  delay(50);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted successfully");
  }

  // Load device ID from EEPROM or LittleFS
  if (!loadDeviceIdFromEEPROM()) {
    loadDeviceIdFromLittleFS();
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    delay(1000);
    ESP.restart();
  }

  printOLED("Pochtomat booting...", "Device ID: " + String(DEVICE_ID));

  // Generate server URLs
  updateServerURLs();

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect("Pochtomat-Setup")) {
    printOLED("WiFi error", "Rebooting...");
    delay(1000);
    ESP.restart();
  }

  timeClient.begin();
  timeClient.update();
  lastNTPUpdate = millis();

  myServo.attach(servoPin);
  myServo.write(0);

  wsClient.onMessage(onWebSocketMessage);
  wsClient.onEvent(onWebSocketEvent);

  // Connect to WebSocket server first
  bool wsSuccess = connectWebSocket();

  // Add a small delay to ensure connection is fully established
  delay(500);

  // If we have a stored device ID, try to register immediately
  if (DEVICE_ID != -1) {
    Serial.println("Using stored Device ID: " + String(DEVICE_ID));
    printOLED("Registering...", "Device ID: " + String(DEVICE_ID));

    // Force registration even if not connected yet
    if (wsSuccess) {
      // Try multiple times to ensure registration happens
      for (int i = 0; i < 3; i++) {
        registerWithWebSocket();
        delay(300);
      }
    } else {
      Serial.println("WebSocket not connected, will register when connected");
    }
  } else {
    // No ID yet, register with server to get one
    Serial.println("No device ID, registering with server...");
    printOLED("Registering...", "Getting device ID");
    tryRegisterDevice();
  }

}

void loop() {
  unsigned long currentMillis = millis();

  // First, handle WebSocket polling if available
  if (wsClient.available()) {
    wsClient.poll();
  }

  // Check if we need to reconnect the WebSocket
  if (!wsConnected) {
    if (currentMillis - lastWsReconnectAttempt >= wsReconnectInterval) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Attempting to reconnect WebSocket");

        if (connectWebSocket()) {
          wsConnected = true;
          reconnectAttempts = 0;
          lastPingTime = currentMillis;
          Serial.println("WebSocket reconnection successful");
        } else {
          reconnectAttempts++;
          Serial.print("WebSocket reconnection failed. Attempt ");
          Serial.println(reconnectAttempts);
        }
      } else {
        Serial.println("WiFi not connected, cannot reconnect WebSocket");
      }
      lastWsReconnectAttempt = currentMillis;
    }
  }
  // Check WebSocket health with ping/pong
  else {
    // Check if we need to send a ping
    if (currentMillis - lastPingTime >= pingInterval) {
      if (wsClient.available()) {
        wsClient.ping();
        Serial.println("Sent WebSocket ping");
        lastPingTime = currentMillis;
      }
    }

    // Check if we've received a pong recently (connection health check)
    if (currentMillis - lastPongTime >= pingInterval * 2) {
      Serial.println("No pong received in too long, connection might be dead");
      wsConnected = false;
      lastWsReconnectAttempt = currentMillis; // Try reconnecting immediately
    }

    // If we're connected but not registered, try to register more frequently
    if (DEVICE_ID != -1 && !registrationSent && currentMillis - lastWebSocketRegistrationAttempt >= wsRegistrationInterval) {
      registerWithWebSocket();
      lastWebSocketRegistrationAttempt = currentMillis;
    }
  }

  // Try to register device if not registered yet - try every second
  if (DEVICE_ID == -1 && currentMillis - lastRegisterTime >= registerInterval) {
    Serial.println("Trying to register device...");
    tryRegisterDevice();
    lastRegisterTime = currentMillis;
  }

  // Update time from NTP server periodically
  if (currentMillis - lastNTPUpdate >= ntpUpdateInterval) {
    timeClient.update();
    lastNTPUpdate = currentMillis;
  }

  // Check PIR sensor for motion
  if (currentMillis - lastPirCheck >= pirCheckInterval) {
    lastPirCheck = currentMillis;

    int pirState = digitalRead(pirPin);
    if (pirState == HIGH && !motionDetected) {
      motionDetected = true;

      if (!doorIsOpen) { // Detect motion only if door is closed
        if (currentMillis - lastMotionSentTime >= motionCooldown) {
          if (wsConnected) {
            String openCommand = "{\"type\":\"command\",\"command\":\"open\"}";
            wsClient.send(openCommand);
            Serial.println("Motion detected, sent open command: " + openCommand);
          } else {
            Serial.println("Motion detected but WebSocket not connected, can't send command");
          }
          sendEvent("new_mail");
          lastMotionSentTime = currentMillis;
        }
    } else if (pirState == LOW && motionDetected) {
      motionDetected = false;
    }
  }

  // Update the display
  if (currentMillis - lastScreenUpdate >= screenUpdateInterval) {
    lastScreenUpdate = currentMillis;
    updateDisplay();
  }

  delay(10); // Small delay to prevent CPU overload
}
