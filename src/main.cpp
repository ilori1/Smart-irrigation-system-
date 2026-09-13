#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ThingSpeak.h>
#include "DHT.h"

// --- Network Credentials ---
const char* WIFI_SSID = "ILORI";        
const char* WIFI_PASS = "11111111";    

// --- ThingSpeak Configuration ---
unsigned long channelID = 3489654; 
const char* writeAPIKey = "PZABE7QNXXVEEIQI"; 

String talkBackID     = "57765";            
String talkBackAPIKey = "NB5U4U4KOORPJ1O6"; 

// --- Calibration Constants ---
// ESP32 Analog Inputs: 4095 = Completely Dry (Air), 1400 = Submerged
const int SOIL_DRY = 4095; 
const int SOIL_WET = 1400; 

// --- Pin Definitions ---
#define DHTPIN 27
#define DHTTYPE DHT11

const int SOIL_SHALLOW_PIN = 34;
const int SOIL_DEEP_PIN    = 35;
const int RAIN_PIN         = 32;
const int RELAY_PIN        = 26;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2); 
WiFiClient client;

enum SystemMode { MODE_AUTO, MODE_FORCE_ON, MODE_FORCE_OFF };
SystemMode currentMode = MODE_AUTO;

bool isPumpActive = false;
unsigned long pumpStartTime = 0;
unsigned long pumpRunSeconds = 0;
unsigned long lastUploadTime = 0;
unsigned long lastTalkBackCheck = 0;

void checkTalkBack() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  
  // Fetch and execute next command in queue
  String url = "https://api.thingspeak.com/talkbacks/" + talkBackID + "/commands/execute?api_key=" + talkBackAPIKey;
  http.begin(url);
  
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

void setup() {
  Serial.begin(115200);

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Irrigation");
  lcd.setCursor(0, 1);
  lcd.print("System Starting");

  // Relay Setup (Active Low Relay: HIGH = OFF, LOW = ON)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); 

  dht.begin();
  
  // Configure ADC Attenuation for GPIO pins 32, 34, 35
  analogSetAttenuation(ADC_11db);
  pinMode(SOIL_SHALLOW_PIN, INPUT);
  pinMode(SOIL_DEEP_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
  lcd.clear();
  lcd.print("WiFi Connected");
  delay(1000);
  lcd.clear();

  ThingSpeak.begin(client);
}

void loop() {
  // 1. Check TalkBack Commands Every 2 Seconds
  if (millis() - lastTalkBackCheck > 2000) {
    checkTalkBack();
    lastTalkBackCheck = millis();
  }

  // 2. Read Raw Analog Values
  int rawShallow = analogRead(SOIL_SHALLOW_PIN);
  int rawDeep    = analogRead(SOIL_DEEP_PIN);
  int rawRain    = analogRead(RAIN_PIN);

  // Map soil percentages safely (Dry=4095 -> 0%, Wet=1400 -> 100%)
  int percentShallow = map(rawShallow, SOIL_DRY, SOIL_WET, 0, 100);
  int percentDeep    = map(rawDeep, SOIL_DRY, SOIL_WET, 0, 100);
  
  percentShallow = constrain(percentShallow, 0, 100);
  percentDeep    = constrain(percentDeep, 0, 100);

  // Rain Module Logic: Dry outputs HIGH (~4095). Wet/Water drops pin LOW (< 2000).
 // Updated: Inverted threshold logic
  bool isRaining = (rawRain > 2000);  
  int avgMoisture = (percentShallow + percentDeep) / 2;

  float tempC = dht.readTemperature();
  float hum   = dht.readHumidity();

  if (isnan(tempC)) tempC = 0.0;
  if (isnan(hum)) hum = 0.0;

  // Print Serial Diagnostics
  Serial.printf("Raw S:%d (%d%%) | Raw D:%d (%d%%) | Raw Rain:%d (Rain:%s) | Mode:%d\n", 
                rawShallow, percentShallow, rawDeep, percentDeep, rawRain, isRaining ? "YES" : "NO", currentMode);

  // 3. Process Pump Logic
  bool prevPumpState = isPumpActive;

  if (currentMode == MODE_FORCE_ON) {
    isPumpActive = true;
  } else if (currentMode == MODE_FORCE_OFF) {
    isPumpActive = false;
  } else { // AUTO_MODE
    if (avgMoisture < 30 && !isRaining) {
      isPumpActive = true;
    } else if (avgMoisture >= 70 || isRaining) {
      isPumpActive = false;
    }
  }

  // Active LOW relay control
  digitalWrite(RELAY_PIN, isPumpActive ? LOW : HIGH);

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
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print((int)tempC);
  lcd.print("C H:");
  lcd.print((int)hum);
  lcd.print("% R:");
  lcd.print(isRaining ? "YES" : "NO ");

  lcd.setCursor(0, 1);
  lcd.print("S:");
  lcd.print(percentShallow);
  lcd.print("% D:");
  lcd.print(percentDeep);
  lcd.print(isPumpActive ? "% ON " : "% OFF");

  // 6. Upload Telemetry to ThingSpeak (Every 15s)
  if (millis() - lastUploadTime > 15000) {
    ThingSpeak.setField(1, tempC);
    ThingSpeak.setField(2, hum);
    ThingSpeak.setField(3, percentShallow);
    ThingSpeak.setField(4, percentDeep);
    ThingSpeak.setField(5, isRaining ? 100 : 0);
    ThingSpeak.setField(6, isPumpActive ? 1 : 0);
    ThingSpeak.setField(7, (long)pumpRunSeconds);
    
    int x = ThingSpeak.writeFields(channelID, writeAPIKey);
    if (x == 200) {
      Serial.println("ThingSpeak Update Successful.");
    } else {
      Serial.println("Problem updating channel. HTTP error code " + String(x));
    }
    lastUploadTime = millis();
  }

  delay(200);
}