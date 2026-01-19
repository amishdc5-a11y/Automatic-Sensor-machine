#include <WiFiS3.h>

const int TRIG_PIN = 9;
const int ECHO_PIN = 10;
const int RELAY_PIN = 7;
const float MAX_DISTANCE = 400;
const float MIN_DISTANCE = 2;
const float DETECTION_THRESHOLD = 200;

unsigned long lastDetectionTime = 0;
unsigned long noDetectionStartTime = 0;
const unsigned long TIMEOUT_DURATION = 4500;
bool lightState = false;
bool wasPresenceDetected = false;
const unsigned long SENSOR_ERROR_TIMEOUT = 10000;

const int NUM_READINGS = 5;
float readings[NUM_READINGS];
int readingIndex = 0;
unsigned long lastValidSensorReading = 0;

// WiFi & ThingSpeak
const char* ssid = "Hotspot";
const char* password = "87654321";
const char* server = "api.thingspeak.com";
const char* apiKey = "NFW77JUMNN5Q8MLE";
WiFiClient client;

unsigned long totalOnTime = 0;
unsigned long lightOnStartTime = 0;
unsigned long lastUpload = 0;

void setup() {
  Serial.begin(9600);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  
  for (int i = 0; i < NUM_READINGS; i++) {
    readings[i] = MAX_DISTANCE;
  }
  
  lastDetectionTime = millis();
  noDetectionStartTime = millis();
  lastValidSensorReading = millis();
  lastUpload = millis();
  
  Serial.println("Smart Room Occupancy System Started");
  
  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    Serial.print(".");
    delay(500);
    tries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Failed - Will retry");
  }
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  
  if (duration == 0) {
    Serial.println("Sensor Error: No echo received");
    return -1;
  }
  
  float distance = duration * 0.034 / 2;
  
  if (distance > MAX_DISTANCE || distance < MIN_DISTANCE) {
    Serial.println("Sensor Error: Distance out of range");
    return -1;
  }
  
  lastValidSensorReading = millis();
  return distance;
}

float getAverageDistance() {
  float sum = 0;
  int validCount = 0;
  
  for (int i = 0; i < NUM_READINGS; i++) {
    if (readings[i] > 0 && readings[i] <= MAX_DISTANCE) {
      sum += readings[i];
      validCount++;
    }
  }
  
  return (validCount > 0) ? sum / validCount : -1;
}

bool detectPresence(float currentDistance) {
  if (millis() - lastValidSensorReading > SENSOR_ERROR_TIMEOUT) {
    Serial.println("Warning: Sensor timeout - assuming no presence");
    return false;
  }
  
  if (currentDistance == -1) {
    float avgDistance = getAverageDistance();
    if (avgDistance > 0 && avgDistance < DETECTION_THRESHOLD) {
      return true;
    }
    return false;
  }
  
  readings[readingIndex] = currentDistance;
  readingIndex = (readingIndex + 1) % NUM_READINGS;
  
  float avgDistance = getAverageDistance();
  
  if (avgDistance > 0 && avgDistance < DETECTION_THRESHOLD) {
    return true;
  }
  
  return false;
}

void controlLight(bool turnOn) {
  if (lightState != turnOn) {
    if (turnOn) {
      lightOnStartTime = millis();
    } else {
      totalOnTime += (millis() - lightOnStartTime);
    }
    
    lightState = turnOn;
    digitalWrite(RELAY_PIN, turnOn ? HIGH : LOW);
    Serial.print(">>> Light: ");
    Serial.println(turnOn ? "ON" : "OFF");
  }
}

void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi disconnected - reconnecting...");
    WiFi.begin(ssid, password);
    return;
  }

  unsigned long currentTotalOnTime = totalOnTime;
  if (lightState) {
    currentTotalOnTime += (millis() - lightOnStartTime);
  }
  
  float minutesOn = currentTotalOnTime / 60000.0;

  if (client.connect(server, 80)) {
    String postStr = apiKey;
    postStr += "&field1=";
    postStr += String(lightState ? 1 : 0);
    postStr += "&field2=";
    postStr += String(currentTotalOnTime);
    postStr += "&field3=";
    postStr += String(minutesOn, 2);
    postStr += "\r\n\r\n";

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + String(apiKey) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(postStr.length());
    client.print("\n\n");
    client.print(postStr);

    delay(500);
    
    while (client.available()) {
      String line = client.readStringUntil('\r');
      if (line.indexOf("200 OK") > 0) {
        Serial.println("✓ Data uploaded successfully!");
      }
    }
    
    client.stop();
  } else {
    Serial.println("✗ ThingSpeak connection failed");
  }
}

void loop() {
  float distance = getDistance();
  
  if (distance > 0) {
    Serial.print("Distance: ");
    Serial.print(distance);
    Serial.print(" cm | Avg: ");
    Serial.print(getAverageDistance());
    Serial.println(" cm");
  }
  
  bool presenceDetected = detectPresence(distance);
  
  if (presenceDetected && !wasPresenceDetected) {
    Serial.println(">>> Presence STARTED");
    lastDetectionTime = millis();
  } else if (!presenceDetected && wasPresenceDetected) {
    Serial.println(">>> Presence ENDED - starting timeout");
    noDetectionStartTime = millis();
  }
  
  wasPresenceDetected = presenceDetected;
  
  if (presenceDetected) {
    controlLight(true);
    Serial.println("Status: Presence detected");
  } else {
    unsigned long noDetectionTime = millis() - noDetectionStartTime;
    
    if (noDetectionTime > TIMEOUT_DURATION && lightState) {
      controlLight(false);
      Serial.println("Status: Timeout reached - Light turned OFF");
    } else if (lightState) {
      Serial.print("Status: No detection - ");
      Serial.print((TIMEOUT_DURATION - noDetectionTime) / 1000);
      Serial.println("s until light off");
    } else {
      Serial.println("Status: No presence detected");
    }
  }
  
  // Upload to ThingSpeak every 20 seconds
  if (millis() - lastUpload > 20000) {
    uploadToThingSpeak();
    lastUpload = millis();
  }
  
  delay(500);
}