/**
 * Project: Smart Health-Tracking Pet Feeder
 * Hardware: ESP32-S3, L298N, HC-SR04, HX711 (x2), DS1302 RTC
 * Author: Tharupathi
 * * LOGIC UPDATE (v2.0):
 * 1. BLE Scanner: Identifies the pet via Collar UUID.
 * 2. 1kg Scale (Bowl): Safety Check. If food remains (>20g), DO NOT dispense.
 * 3. 5kg Scale (Platform): Health Check. Logs pet weight while they eat.
 */

#include <WiFi.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <RtcDS1302.h> 
#include "HX711.h"      

// ==========================================
// 1. CONFIGURATION & CREDENTIALS
// ==========================================
const char* ssid     = "SSID";      // <--- ENTER WIFI NAME
const char* password = "Password";  // <--- ENTER WIFI PASSWORD
const long  gmtOffset_sec = 19800;  // GMT +5:30 (Sri Lanka/India standard)
const int   daylightOffset_sec = 0;

// ==========================================
// 2. PIN DEFINITIONS
// ==========================================
// Motor Driver (L298N)
#define IN1 4  
#define IN2 5  
#define RGB_BRIGHTNESS 50 

// Ultrasonic Sensor (Hopper Level)
#define TRIG_PIN 10
#define ECHO_PIN 11

// 5KG Scale -> PET WEIGHT (Platform under the feeder)
#define DOUT_5KG 12
#define SCK_5KG  13

// 1KG Scale -> BOWL CHECK (Weighs the food bowl directly)
#define DOUT_1KG 14
#define SCK_1KG  21

// RTC Module (DS1302)
ThreeWire myWire(7, 6, 15); 
RtcDS1302<ThreeWire> Rtc(myWire);

// ==========================================
// 3. GLOBAL VARIABLES
// ==========================================
HX711 scalePet;   // Associated with 5KG Load Cell
HX711 scaleBowl;  // Associated with 1KG Load Cell

float lastMeasuredDistance = 0.0; 
int fillLevel = 0; 

// Calibration Factors 
// IMPORTANT: You must run a calibration sketch to find these values for your specific load cells
float cal_pet_platform = 384;    // Factor for 5kg Load Cell
float cal_bowl = 2068.3;         // Factor for 1kg Load Cell

// Thresholds
float leftoverThreshold = 20.0;  // Grams. If bowl has more than this, we skip dispensing.

int lastResetHour = -1; 
int scanTime = 1; // Duration (seconds) to scan for BLE devices
BLEScan* pBLEScan;

// ==========================================
// 4. USER MANAGEMENT
// ==========================================
struct AuthorizedDevice {
    String name;
    String uuid; // Service UUID broadcast by the Collar ESP32
};

AuthorizedDevice userList[5] = {
    {"", ""}, //{"Name", "UUID"}
    {"", ""}, {"", ""}, {"", ""}, {"", ""}  
};

float weightLogs[5] = {0.0, 0.0, 0.0, 0.0, 0.0};          
BLEUUID authorizedUUIDs[5];                               
bool motorFiredForDevice[5] = {false, false, false, false, false}; // Tracks if pet has eaten this meal block
bool deviceDetectedNow[5] = {false, false, false, false, false};   // Immediate detection flag

// ==========================================
// 5. HELPER FUNCTIONS
// ==========================================

// Measure Hopper Fill Level
float readUltrasonic() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
  return (duration == 0) ? 0 : (duration * 0.0343) / 2;
}

// BLE Callback: Handles detection of "Smart Collar"
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        if (advertisedDevice.haveServiceUUID()) {
            for (int i = 0; i < 5; i++) {
                // Check if the broadcast UUID matches our authorized list
                if (userList[i].uuid == "" || !advertisedDevice.isAdvertisingService(authorizedUUIDs[i])) continue;
                
                // RSSI Check: -70 ensures the pet is actually CLOSE to the feeder, not just in the room
                if (advertisedDevice.getRSSI() > -70) {
                    deviceDetectedNow[i] = true; 
                }
            }
        }
    }
};

// ==========================================
// 6. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Pin Initialization
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW); 
  neopixelWrite(RGB_BUILTIN, 0, 0, 0); 

  Serial.println("Initializing Scales...");
  
  // --- 5KG SCALE SETUP (Pet Weight) ---
  scalePet.begin(DOUT_5KG, SCK_5KG); 
  scalePet.set_scale(cal_pet_platform); 
  scalePet.tare(); // Assumes platform is empty on boot

  // --- 1KG SCALE SETUP (Bowl Check) ---
  scaleBowl.begin(DOUT_1KG, SCK_1KG); 
  scaleBowl.set_scale(cal_bowl); 
  scaleBowl.tare(); // Assumes bowl is empty on boot

  Rtc.Begin();
  
  // --- TIME SYNC (NTP) ---
  Serial.print("Connecting to WiFi for Time Sync...");
  WiFi.begin(ssid, password);
  int retry = 0; 
  while (WiFi.status() != WL_CONNECTED && retry < 10) { delay(500); retry++; Serial.print("."); }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! Fetching NTP time...");
    configTime(gmtOffset_sec, 0, "pool.ntp.org");
    struct tm info;
    if (getLocalTime(&info)) {
      Rtc.SetDateTime(RtcDateTime(info.tm_year+1900, info.tm_mon+1, info.tm_mday, info.tm_hour, info.tm_min, info.tm_sec));
      Serial.println("RTC Updated.");
    }
    // Turn off WiFi to save power and reduce interference with BLE/Scales
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); 
  } else {
    Serial.println("\nWiFi Failed. Using stored RTC time.");
  }

  // BLE Initialization
  for (int i = 0; i < 5; i++) {
      if (userList[i].uuid != "") authorizedUUIDs[i] = BLEUUID(userList[i].uuid.c_str());
  }

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); 
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
  pBLEScan->setActiveScan(true); 
  Serial.println("System Ready. Scanning for Collars...");
}

// ==========================================
// 7. MAIN LOOP
// ==========================================
void loop() {
  RtcDateTime now = Rtc.GetDateTime();

  // --- SCHEDULE LOGIC ---
  // Resets the "Fed" flag (motorFiredForDevice) at meal times (6am, 11am, 6pm)
  if ((now.Hour() == 6 || now.Hour() == 11 || now.Hour() == 18) && now.Minute() == 0) {
    if (lastResetHour != now.Hour()) {
      Serial.printf(">>> SCHEDULED RESET: Flags cleared at %02u:00 <<<\n", now.Hour());
      for (int i = 0; i < 5; i++) motorFiredForDevice[i] = false; 
      lastResetHour = now.Hour(); 
    }
  } else {
    lastResetHour = -1; 
  }

  // --- BLE SCANNING ---
  // Reset detection flags and scan for 1 second
  for(int i = 0; i < 5; i++) deviceDetectedNow[i] = false;
  pBLEScan->start(scanTime, false);
  
  // --- PROCESSING LOGIC ---
  for(int i = 0; i < 5; i++) {
    // If Pet is identified nearby AND hasn't been fed this meal period yet
    if (deviceDetectedNow[i] && !motorFiredForDevice[i]) {
          
          Serial.printf("[%02u:%02u:%02u] ID Detected: %s\n", now.Hour(), now.Minute(), now.Second(), userList[i].name.c_str());
          
          // ==================================================
          // STEP 1: LEFTOVER CHECK (1KG Scale)
          // ==================================================
          // We check the bowl weight before dispensing to prevent overflow/waste.
          float currentFoodWeight = scaleBowl.get_units(5); // Average 5 readings
          Serial.printf("Bowl Status: %.2f g\n", currentFoodWeight);

          if (currentFoodWeight > leftoverThreshold) {
            // Logic: There is still food in the bowl.
            Serial.println(">>> SKIPPING MEAL: Bowl still has food.");
          } 
          else {
            // Logic: Bowl is empty (or low enough). Proceed to feed.
            Serial.println(">>> BOWL EMPTY: Dispensing...");

            // ==================================================
            // STEP 2: DISPENSE SEQUENCE
            // ==================================================
            // Anti-jamming: Rotates forward then backward
            
            // Cycle 1
            digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); delay(1000); 
            digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH); delay(500); 
            
            // Cycle 2
            digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); delay(1000); 
            digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH); delay(500); 

            // Stop Motor
            digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
          }
          
          // ==================================================
          // STEP 3: HEALTH MONITORING (5KG Scale)
          // ==================================================
          // While the food is dispensed (or skipped), the pet is likely standing 
          // on the platform. We measure their weight now.
          Serial.println("Acquiring Pet Health Data...");
          delay(2000); // Wait for pet to settle on platform
          float currentPetWeight = scalePet.get_units(20); // Average 20 readings for stability
          
          // ==================================================
          // STEP 4: SYSTEM LOGGING
          // ==================================================
          lastMeasuredDistance = readUltrasonic();
          fillLevel = map((int)lastMeasuredDistance, 29, 15, 0, 100); //Change According to distance
          fillLevel = constrain(fillLevel, 0, 100);

          Serial.printf(">>> REPORT: Hopper Level: %d%%\n", fillLevel);
          Serial.printf(">>> REPORT: Pet Weight Logged: %.2f g\n", currentPetWeight);
          
          // Mark this pet as "Handled" for this time block so we don't dispense again immediately
          motorFiredForDevice[i] = true; 
    }
  }

  // Visual Indicator (Blue LED turns ON if ANY pet is currently detected via BLE)
  bool anyUserPresent = false;
  for(int i=0; i<5; i++) if(deviceDetectedNow[i]) anyUserPresent = true;
  if (anyUserPresent) neopixelWrite(RGB_BUILTIN, 0, 0, RGB_BRIGHTNESS); 
  else neopixelWrite(RGB_BUILTIN, 0, 0, 0); 

  pBLEScan->clearResults(); 
}
