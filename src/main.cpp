#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ThingSpeak.h>
#include "DHT.h"

// --- Function Prototypes ---
void checkTalkBack();

// --- Network Credentials ---
const char* WIFI_SSID = "ILORI";        
const char* WIFI_PASS = "11111111";    

// --- ThingSpeak Configuration ---
unsigned long channelID = 3489654; 
const char* writeAPIKey = "PZABE7QNXXVEEIQI"; 

String talkBackID     = "57765";            
String talkBackAPIKey = "NB5U4U4KOORPJ1O6"; 

// --- Calibration Constants ---
const int SOIL_DRY = 4095; 
const int SOIL_WET = 1400; 

// --- Pin Definitions ---
#define DHTPIN 27
#define DHTTYPE DHT11

const int SOIL_SHALLOW_PIN = 34;
const int SOIL_DEEP_PIN    = 35;
const int RAIN_PIN         = 32;
const int RELAY_PIN        = 26;
const bool RELAY_ACTIVE_LOW = true;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2); 
WiFiClient client;

enum SystemMode { MODE_AUTO, MODE_FORCE_ON, MODE_FORCE_OFF };
SystemMode currentMode = MODE_AUTO;

bool isPumpActive = false;
bool lastRelayState = false;
unsigned long pumpStartTime = 0;
unsigned long pumpRunSeconds = 0;
unsigned long lastUploadTime = 0;
unsigned long lastTalkBackCheck = 0;
unsigned long lastWiFiReconnect = 0;

void setup() {
  Serial.begin(115200);

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Irrigation");
  lcd.setCursor(0, 1);
  lcd.print("Booting System..");

  // Relay Setup (Set default state to OFF)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);

  dht.begin();
  
  // Configure ADC Attenuation
  analogSetAttenuation(ADC_11db);
  pinMode(SOIL_SHALLOW_PIN, INPUT);
  pinMode(SOIL_DEEP_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);

  // Attempt initial quick connection
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(300);
    attempts++;
  }

  lcd.clear();
  ThingSpeak.begin(client);
}

void checkTalkBack() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String url = "https://api.thingspeak.com/talkbacks/" + talkBackID + "/commands/execute?api_key=" + talkBackAPIKey;
  
  http.begin(url);
  http.setTimeout(1200);
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    payload.trim();
    
    if (payload.length() > 0) {
      Serial.print("[TalkBack Executed]: ");
      Serial.println(payload);
      
      if (payload.indexOf("TURN_ON") >= 0) {
        currentMode = MODE_FORCE_ON;
      } else if (payload.indexOf("TURN_OFF") >= 0) {
        currentMode = MODE_FORCE_OFF;
      } else if (payload.indexOf("AUTO_MODE") >= 0) {
        currentMode = MODE_AUTO;
      }
    }
  }
  http.end();
}

void setPumpRelay(bool pumpOn) {
  const uint8_t relayOnLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t relayOffLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_PIN, pumpOn ? relayOnLevel : relayOffLevel);

  if (pumpOn != lastRelayState) {
    Serial.print("Pump ");
    Serial.print(pumpOn ? "ON" : "OFF");
    Serial.print("; GPIO26=");
    Serial.println(pumpOn ? relayOnLevel : relayOffLevel);
    lastRelayState = pumpOn;
  }
}

void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // Non-blocking background reconnect every 10 seconds if connection is lost
  if (!isConnected && (millis() - lastWiFiReconnect > 10000)) {
    WiFi.reconnect();
    lastWiFiReconnect = millis();
  }

  // 1. Check TalkBack Commands
  if (isConnected && (millis() - lastTalkBackCheck > 2000)) {
    checkTalkBack();
    lastTalkBackCheck = millis();
  }

  // 2. Read Sensors
  int rawShallow = analogRead(SOIL_SHALLOW_PIN);
  int rawDeep    = analogRead(SOIL_DEEP_PIN);
  int rawRain    = analogRead(RAIN_PIN);

  int percentShallow = map(rawShallow, SOIL_DRY, SOIL_WET, 0, 100);
  int percentDeep    = map(rawDeep, SOIL_DRY, SOIL_WET, 0, 100);
  
  percentShallow = constrain(percentShallow, 0, 100);
  percentDeep    = constrain(percentDeep, 0, 100);

  bool isRaining = (rawRain > 2000);  
  int avgMoisture = (percentShallow + percentDeep) / 2;

  float tempC = dht.readTemperature();
  float hum   = dht.readHumidity();

  if (isnan(tempC)) tempC = 0.0;
  if (isnan(hum)) hum = 0.0;

  // 3. Process Irrigation Logic
  bool prevPumpState = isPumpActive;

  if (currentMode == MODE_FORCE_ON) {
    isPumpActive = true;
  } else if (currentMode == MODE_FORCE_OFF) {
    isPumpActive = false;
  } else { // MODE_AUTO
    if (avgMoisture < 30 && !isRaining) {
      isPumpActive = true;
    } else if (avgMoisture >= 70 || isRaining) {
      isPumpActive = false;
    }
  }

  // Set RELAY_ACTIVE_LOW to false for relay boards triggered by HIGH.
  setPumpRelay(isPumpActive);

  // 4. Runtime Calculation
  if (isPumpActive) {
    if (!prevPumpState) {
      pumpStartTime = millis();
    }
    pumpRunSeconds = (millis() - pumpStartTime) / 1000;
  } else {
    pumpRunSeconds = 0;
  }

  // 5. Update LCD Screen
  // Line 1: Temp, Humidity, and WiFi Status (e.g. "T:28C H:60% W:OK")
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print((int)tempC);
  lcd.print("C H:");
  lcd.print((int)hum);
  lcd.print("% W:");
  lcd.print(isConnected ? "OK" : "NO");

  // Line 2: Soil Moisture, Rain Status, and Pump State (e.g. "S:45% R:NO P:ON ")
  lcd.setCursor(0, 1);
  lcd.print("S:");
  lcd.print(avgMoisture);
  lcd.print("% R:");
  lcd.print(isRaining ? "YES" : "NO ");
  lcd.print(" P:");
  lcd.print(isPumpActive ? "ON " : "OFF");

  // 6. Upload Telemetry to ThingSpeak (Every 15s)
  if (millis() - lastUploadTime > 15000) {
    if (isConnected) {
      ThingSpeak.setField(1, tempC);
      ThingSpeak.setField(2, hum);
      ThingSpeak.setField(3, percentShallow);
      ThingSpeak.setField(4, percentDeep);
      ThingSpeak.setField(5, isRaining ? 100 : 0);
      ThingSpeak.setField(6, isPumpActive ? 1 : 0);
      ThingSpeak.setField(7, (long)pumpRunSeconds);
      
      int x = ThingSpeak.writeFields(channelID, writeAPIKey);
      if (x == 200) {
        Serial.println("ThingSpeak Upload Successful.");
      } else {
        Serial.println("ThingSpeak Upload Error Code: " + String(x));
      }
    } else {
      Serial.println("Skipped Upload: WiFi Disconnected.");
    }
    lastUploadTime = millis();
  }

  delay(200);
}