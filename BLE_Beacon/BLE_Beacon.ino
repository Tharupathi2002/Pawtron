#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// This is going to be the uuid  main unit searches for (make sure to use this uuid in the main code too)
#define SERVICE_UUID "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" //use an online uuid generator service and generate one

void setup() {
  Serial.begin(115200);
  Serial.println("Starting BLE Beacon: kitty");

  // Initializing the BLE device with the name as kitty for a example
  BLEDevice::init("kitty");

  //creating a BLE server
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pService->start();

  // Adverising (showing to main device)
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  
  // this part of code is used for showing the beacons uuid to a scanner (in this case pawtron main unit)
  pAdvertising->addServiceUUID(SERVICE_UUID);
  
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); 
  pAdvertising->setMinPreferred(0x12);
  
  // starting advertising
  BLEDevice::startAdvertising();
  Serial.println("Beacon is now broadcasting...");
}

void loop() {
  delay(2000);
}