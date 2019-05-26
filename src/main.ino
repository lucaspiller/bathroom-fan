#include <SimpleDHT.h>
#include <ArduinoJson.h>
#include <CircularBuffer.h>
#include <MQTT.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "config.h"

#define DHT_PIN D5
#define FAN_PIN D7

#define DHT_INTERVAL_SECONDS 5
#define CONTROL_INTERVAL_SECONDS 1
#define HISTORY_INTERVAL_SECONDS 15

#define HISTORY_LENGTH 720 // 3 hours

SimpleDHT22 dht22;
ESP8266WebServer server(80);
DynamicJsonBuffer jsonBuffer;

WiFiClient net;
MQTTClient mqtt;

unsigned long historyLastUpdateTime = 0;
CircularBuffer<bool,HISTORY_LENGTH> historyFan;
CircularBuffer<byte,HISTORY_LENGTH> historyTemperature;
CircularBuffer<byte,HISTORY_LENGTH> historyHumidity;

unsigned long dhtLastUpdateTime = 0;
bool dhtError = false;
byte temperature = 0;
byte humidity = 0;

unsigned long controlLastUpdateTime = 0;
bool fanRunning = false;
bool manualOverride = false;

//
// DHT
//
void dhtRead() {
  byte newTemperature;
  byte newHumidity;
  int err = dht22.read(DHT_PIN, &newTemperature, &newHumidity, NULL);
  if (err != SimpleDHTErrSuccess) {
    dhtError = true;
    Serial.print("Read dht22 failed, err=");
    Serial.println(err);
  } else {
    temperature = newTemperature;
    humidity = newHumidity;
    dhtError = false;
  }
}

//
// Fan Control
//
void fanStart() {
  fanRunning = true;
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(FAN_PIN, HIGH);
}

void fanStop() {
  fanRunning = false;
  pinMode(LED_BUILTIN, INPUT);
  digitalWrite(FAN_PIN, LOW);
}

//
// Automatic Control
//
void controlLoop() {
  // Manual
  if (manualOverride) {
    fanStart();
    return;
  }

  // Stop if DHT fails
  if (dhtError) {
    fanStop();
    return;
  }

  // Automatic Control
  if (fanRunning) {
    if (humidityDelta(15) <= FIFTEEN_MINUTE_STOP_DELTA) {
      fanStop();
      return;
    }

    if (fanRuntimeMinutes() > MAX_FAN_RUNTIME_MINUTES) {
      fanStop();
      return;
    }
  } else {
    if (humidity >= START_HUMIDITY) {
      fanStart();
      return;
    }

    if (humidityDelta(1) >= ONE_MINUTE_START_DELTA) {
      fanStart();
      return;
    }

    if (humidityDelta(5) >= FIVE_MINUTE_START_DELTA) {
      fanStart();
      return;
    }
  }
}

//
// History
//
void historyLoop() {
  historyFan.unshift(fanRunning);
  historyTemperature.unshift(temperature);
  historyHumidity.unshift(humidity);
  mqttPublish();
}

int humidityDelta(int minutes) {
  unsigned int entry = (60 * minutes) / HISTORY_INTERVAL_SECONDS;

  int last;
  if (historyHumidity.size() < entry) {
    last = (int) historyHumidity.last();
  } else {
    last = (int) historyHumidity[entry - 1];
  }

  return ((int) historyHumidity[0]) - last;
}

int fanRuntimeMinutes() {
  if (!historyFan[0]) {
    return 0;
  }

  unsigned int last = historyFan.size();
  for(unsigned int i = 0; i < historyFan.size(); i++) {
    if (!historyFan[i]) {
      last = i;
      break;
    }
  }

  return (last * HISTORY_INTERVAL_SECONDS) / 60;
}

//
// Web Server
//
void serverRoot() {
  char temp[50];
  String resp = "<html>\r\n";
  resp += "<body>\r\n";
  resp += "<p>\r\n";
  resp += "Mode:\r\n";

  if (manualOverride) {
    resp += "<strong>MANUAL</strong>\r\n";
    resp += "<a href=\"/manual/cancel\"><button>Switch</button></a>\r\n";
  } else {
    resp += "<strong>AUTO</strong>\r\n";
    resp += "<a href=\"/manual/start\"><button>Switch</button></a>\r\n";
  }

  resp += "</p>\r\n";

  resp += "<p>\r\n";
  resp += "Fan:\r\n";
  if (fanRunning) {
    resp += "<strong>Running</strong>";
    sprintf(temp, " (%01d minutes)\r\n", fanRuntimeMinutes());
    resp += temp;
  } else {
    resp += "<strong>Stopped</strong>\r\n";
  }
  resp += "</p>\r\n";

  resp += "<p>\r\n";
  resp += "Sensor:\r\n";
  if (dhtError) {
    resp += "<strong>Error</strong>";
  } else {
    resp += "<strong>OK</strong>";
  }

  sprintf(temp, "<br/>Temperature: %1.0d&deg;C\r\n", (int) temperature);
  resp += temp;
  sprintf(temp, "<br/>Humidity: %1.0d%%\r\n", (int) humidity);
  resp += temp;
  sprintf(temp, "<br/>Delta: %01d %01d %01d\r\n", humidityDelta(1), humidityDelta(5), humidityDelta(15));
  resp += temp;
  resp += "</p>\r\n";

  resp += "<p>\r\n";
  resp += "MQTT:\r\n";
  if (mqtt.connected()) {
    resp += "<strong>Connected</strong>";
  } else {
    resp += "<strong>Error</strong>";
  }
  sprintf(temp, "<br/>lastError: %01d\r\n", mqtt.lastError());
  resp += temp;
  sprintf(temp, "<br/>returnCode: %01d\r\n", mqtt.returnCode());
  resp += temp;
  resp += "</p>\r\n";
  resp += "</body>\r\n";
  resp += "</html>\r\n";

  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "text/html", resp);
}

void serverHistory() {
  JsonObject& root = jsonBuffer.createObject();
  JsonArray& history = root.createNestedArray("history");

  for (int i = 0; i < historyFan.size(); i++) {
    JsonObject& result = jsonBuffer.createObject();
    result["fanRunning"] = historyFan[i] ? 1 : 0;
    result["temperature"] = (int) historyTemperature[i];
    result["humidity"] = (int) historyHumidity[i];
    history.add(result);
  }

  String resp = "";
  root.printTo(resp);

  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "application/json", resp);
}

String buildStatus() {
  JsonObject& root = jsonBuffer.createObject();
  if (manualOverride) {
    root["mode"] = "manual";
  } else {
    root["mode"] = "auto";
  }

  root["fanRunning"] = fanRunning;
  root["dhtError"] = dhtError;
  root["temperature"] = (int) temperature;
  root["humidity"] = (int) humidity;

  String json = "";
  root.printTo(json);
  return json;
}

void serverStatus() {
  String status = buildStatus();
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", status);
}

void serverManualStart() {
  manualOverride = true;

  if ( server.method() == HTTP_GET ) {
    server.sendHeader("Location","/");
    server.sendHeader("Cache-Control","no-cache");
    server.send(302);
  } else {
    server.send(201);
  }
}

void serverManualCancel() {
  manualOverride = false;

  if ( server.method() == HTTP_GET ) {
    server.sendHeader("Location","/");
    server.sendHeader("Cache-Control","no-cache");
    server.send(302);
  } else {
    server.send(201);
  }
}

void serverStart() {
  server.on("/", serverRoot);
  server.on("/history", serverHistory);
  server.on("/manual/start", serverManualStart);
  server.on("/manual/cancel", serverManualCancel);
  server.on("/status", serverStatus);
  server.begin();
}

//
// MQTT
//
void mqttStart() {
  mqtt.begin(MQTT_HOST, net);
  mqtt.connect("bathroom-fan");
}

void mqttPublish() {
  if (!mqtt.connected()) {
    if (!mqtt.connect("bathroom-fan")) {
      return;
    }
  }

  String status = buildStatus();
  mqtt.publish(MQTT_TOPIC_STATUS, status);
}

//
// OTA Update
//
void otaStart() {
  ArduinoOTA.setHostname("bathroom-fan");
  ArduinoOTA.onStart([]() {
    fanStop();

    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    delay(1000);
    ESP.restart();
  });
  ArduinoOTA.begin();
}

//
// Main Program
//
void setup() {
  Serial.begin(115200);
  Serial.println(F("== Bathroom Fan =="));

  pinMode(DHT_PIN, INPUT);
  pinMode(LED_BUILTIN, INPUT); // LED off - if it's set to an output it will be on
  pinMode(FAN_PIN, OUTPUT);

  // Connect to wifi
  WiFi.hostname("bathroom-fan");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Waiting for wifi...");

  while (WiFi.status() != WL_CONNECTED) {
    pinMode(LED_BUILTIN, INPUT);
    delay(200);
    pinMode(LED_BUILTIN, OUTPUT);
    delay(200);
    Serial.print(".");
  }

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Start
  serverStart();
  mqttStart();
  otaStart();
}

void loop() {
  // DHT
  if ((unsigned long)(millis() - dhtLastUpdateTime) > DHT_INTERVAL_SECONDS * 1000) {
    dhtRead();
    dhtLastUpdateTime = millis();
  }

  // Control
  if ((unsigned long)(millis() - controlLastUpdateTime) > CONTROL_INTERVAL_SECONDS * 1000) {
    controlLoop();
    controlLastUpdateTime = millis();
  }

  // History
  if ((unsigned long)(millis() - historyLastUpdateTime) > HISTORY_INTERVAL_SECONDS * 1000) {
    historyLoop();
    historyLastUpdateTime = millis();
  }


  server.handleClient();
  ArduinoOTA.handle();
  mqtt.loop();
  delay(100);
}
