#include "connectionDetails.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>

#define WELL_LEVEL_SENSOR D5
const int RELAY_SWITCH = D7;

WiFiClient espClient;
PubSubClient client(espClient);

//Weather
#define SEALEVELPRESSURE_HPA (1021)
Adafruit_BME280 bme;
float temperature, humidity, pressure, altitude;

unsigned long timeStopped = 0;
unsigned long timeStarted = 0;
bool startingMode = true;
bool wellFull = false;
bool pumpState = false;
String waterTankIntent = "OFF";
String delayStart = "";
int delayStartInt = 1800000; //Half hr - Debounce for when well signals enough water. Pump needs to wait or else it wil stop/start in quick succession.
int minuteCountdown = 0;
unsigned long recordSeconds = 0;
int keepAliveInterval = 5000;

const char* timeToWaitTopic = "water-well/recovery-time";
const char* waterWellStatusTopic = "water-well/status";
const char* waterTankIntentTopic = "water-tank/status";
const char* waterPumpStatusTopic = "water-pump/status";
const char* waterPumpAvailabilityTopic = "water-pump/availability";
const char* waterWellRecoveryCountdownTopic = "water-well/recovery-countdown";
const char* waterWellRestartTopic = "water-well/restart";
const char* waterWellWeatherTopic = "water-well/weather";

void setupWifi() {
  //Wait 10 seconds. This is to ensure the keepalives being sent to homeassistant stop, as to make the availability topic expire
  //and set the board as offline. When it goes back online, an offline to online state change is detected which homeassistant can use as a trigger
  //to resent the previously set state for water well recovery time.
  delay(10000);
  // Set hostname
  WiFi.hostname("water-pump-controller");
  // Start by connecting to a WiFi network
  Serial.println("Connecting to: " + String(ssid));
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("Message arrived [" + String(topic) + "]");
  Serial.println(topic);
  if (strcmp(topic, waterTankIntentTopic) == 0) {
    waterTankIntent = "";
    for (int i = 0; i < length; i++) {
      waterTankIntent += (char)payload[i];
    }
    Serial.println("Received " + String(waterTankIntentTopic) + " " + waterTankIntent);
  } else if (strcmp(topic, timeToWaitTopic) == 0) {
    delayStart = "";
    for (int i = 0; i < length; i++) {
      delayStart += (char)payload[i];
    }
    delayStartInt = delayStart.toInt() * 60000;
    Serial.println("Time to start " + String(delayStartInt));
    client.publish(waterWellRecoveryCountdownTopic, delayStart.c_str());
    Serial.println("Received " + String(timeToWaitTopic) + " " + delayStart);

    //Reset last time stopped to now
    timeStopped = millis();
    timeStarted = 0;
    minuteCountdown = 0;
    if (pumpState) {  //And if the pump is already running. Stop it.
      setPumpState(false, String(delayStart));
    }
  } else if (strcmp(topic, waterWellRestartTopic) == 0) {
    ESP.restart();
  }
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(), mqttUser, mqttPassword)) {
      Serial.println("connected");
      client.subscribe(waterTankIntentTopic);
      client.subscribe(timeToWaitTopic);
      client.subscribe(waterWellRestartTopic);
      // Initial waiting time is set as default in case MQTT server is down.
      Serial.println("About to set initial waiting time");
      client.publish(timeToWaitTopic, String(delayStartInt / 60000).c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void readWeatherData() {
  temperature = bme.readTemperature();
  humidity = bme.readHumidity();
  pressure = bme.readPressure() / 100.0F;
  altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);
  //  Serial.print("Temperature");
  //  Serial.println(temperature);
  //  Serial.print("humidity");
  //  Serial.println(humidity);
  //  Serial.print("pressure");
  //  Serial.println(pressure);
  //  Serial.print("altitude");
  //  Serial.println(altitude);

  StaticJsonDocument<200> doc;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["pressure"] = pressure;
  doc["altitude"] = altitude;
  String output;
  serializeJson(doc, output);
  client.publish(waterWellWeatherTopic, output.c_str());
}

void setup() {
  // Initialize the LED_BUILTIN pin as an output
  pinMode(LED_BUILTIN, OUTPUT);
  // set pin as input
  pinMode(WELL_LEVEL_SENSOR, INPUT_PULLUP);
  // set pin as output
  pinMode(RELAY_SWITCH, OUTPUT);
  //Start light as off. High is use because current needs to flow through, to turn off
  digitalWrite(LED_BUILTIN, HIGH);
  bme.begin(0x76);
  Serial.begin(9600);
  setupWifi();
  client.setServer(mqttServer, 1883);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    setPumpState(false, String(delayStart));
    reconnect();
  }
  client.loop();

  keepAlive();
  calculatePumpState();
  if (startingMode) {
    startMode();
    startingMode = false;
  }
}

void startMode() {
  //If the water well level is low
  if (digitalRead(WELL_LEVEL_SENSOR) == HIGH) {
    client.publish(waterWellStatusTopic, "EMPTY");
  } else {
    client.publish(waterWellStatusTopic, "FULL");
  }
  const String stateStr = pumpState ? "ON" : "OFF";
  client.publish(waterPumpStatusTopic, stateStr.c_str());
  client.publish(waterWellRecoveryCountdownTopic, delayStart.c_str());
}

void calculatePumpState() {
  if (digitalRead(WELL_LEVEL_SENSOR) == HIGH)  //If the water well level is low
  {
    timeStopped = millis();
    if (pumpState) {  //And the pump is already running
      setPumpState(false, delayStart);
    }
    if (wellFull) {
      //Not enough water in well
      client.publish(waterWellStatusTopic, "EMPTY");
      client.publish(waterWellRecoveryCountdownTopic, delayStart.c_str());
      wellFull = false;
    }
  }
  else //If there is enough water in the well
  {
    timeStarted = millis();
    //If the pump is running
    if (pumpState) {
      if (waterTankIntent == "OFF" || waterTankIntent != "ON") { //Water tank is full. If anything else other than "ON or OFF" is sent. Switch off
        setPumpState(false, "0");
      }
    } else { //if pump not running
      if ((timeStarted - timeStopped) >= delayStartInt) { //And the delay counter is over, then switch pump on
        if (waterTankIntent == "ON") { //Water tank is empty
          setPumpState(true, "0");
        }
      } else { //Send how long it will be until pump started every minute
        int timeToWait = ((delayStartInt - (timeStarted - timeStopped)) / 60000);
        if (timeToWait != minuteCountdown) {
          minuteCountdown = timeToWait;
          Serial.println("Starting in: " + String(minuteCountdown) + " minutes");
          client.publish(waterWellRecoveryCountdownTopic, String(minuteCountdown).c_str());
        }
      }
    }
    if (!wellFull) {
      //Not enough water in well
      client.publish(waterWellStatusTopic, "FULL");
      client.publish(waterWellRecoveryCountdownTopic, delayStart.c_str());
      wellFull = true;
    }
  }
}

void setPumpState(bool state, String wellTimeToRecover) {
  Serial.println("STATE " + String(state));
  Serial.println("wellTimeToRecover " + wellTimeToRecover);
  pumpState = state;
  const String stateStr = state ? "ON" : "OFF";
  Serial.println("Pump " + stateStr);
  digitalWrite(LED_BUILTIN, (state ? LOW : HIGH));
  client.publish(waterPumpStatusTopic, stateStr.c_str());
  client.publish(waterWellRecoveryCountdownTopic, wellTimeToRecover.c_str());
  digitalWrite(RELAY_SWITCH, (state ? HIGH : LOW));
}

void keepAlive() {
  if ((millis() - recordSeconds) >= keepAliveInterval) {
    client.publish(waterPumpAvailabilityTopic, "ON");
    recordSeconds = millis();
    readWeatherData();
  }
}
