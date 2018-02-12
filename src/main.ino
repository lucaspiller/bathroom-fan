#include <SimpleDHT.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "config.h"

#define DHT_PIN D5
#define FAN_PIN D7

#define DHT_INTERVAL 5000 // 5 seconds
#define CONTROL_INTERVAL 1000 // 1 second
#define HISTORY_INTERVAL 60000 // 1 minute

#define HISTORY_LENGTH 120 // 2 hours

SimpleDHT22 dht22;
ESP8266WebServer server(80);

bool historyFan[HISTORY_LENGTH];
byte historyTemperature[HISTORY_LENGTH];
byte historyHumidity[HISTORY_LENGTH];

int historyPointer = 0;
int historyLastUpdateTime = 0;

int dhtLastUpdateTime = 0;
bool dhtError = false;
byte temperature = 0;
byte humidity = 0;

int controlLastUpdateTime = 0;
bool fanRunning = false;
bool manualOverride = false;

//
// DHT
//
void dhtRead() {
  int err = dht22.read(DHT_PIN, &temperature, &humidity, NULL);
  if (err != SimpleDHTErrSuccess) {
    dhtError = true;
    Serial.print("Read dht22 failed, err=");
    Serial.println(err);
  } else {
    dhtError = false;
  }
}

//
// Fan Control
//
void fanStart() {
  fanRunning = true;
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(FAN_PIN, HIGH);
}

void fanStop() {
  fanRunning = false;
  pinMode(BUILTIN_LED, INPUT);
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
    if (humidity <= STOP_HUMIDITY) {
      fanStop();
    }
  } else {
    if (humidity >= START_HUMIDITY) {
      fanStart();
    }
  }
}

//
// History
//
void historyLoop() {
  historyFan[historyPointer] = fanRunning;
  historyTemperature[historyPointer] = temperature;
  historyHumidity[historyPointer] = humidity;

  historyPointer++;
  if (historyPointer == HISTORY_LENGTH) {
    historyPointer = 0;
  }
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
    resp += "<strong>Running</strong>\r\n";
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
  sprintf(temp, "<br/>Humidity: %1.0d%%", (int) humidity);
  resp += temp;
  resp += "</p>\r\n";
  resp += "<img src=\"graph.svg\"/>\r\n";
  resp += "</body>\r\n";
  resp += "</html>\r\n";

  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "text/html", resp);
}

void serverHistory() {
  char temp[50];
  String resp = "<html>\r\n";
  resp += "<body>\r\n";
  resp += "<table>\r\n";
  resp += "<tr>\r\n";
  resp += "<th>Fan</th>\r\n";
  resp += "<th>Temperature</th>\r\n";
  resp += "<th>Humidity</th>\r\n";
  resp += "</tr>\r\n";

  for (int i = 1; i < HISTORY_LENGTH; i++) {
    int ix = historyPointer - i;
    if (ix < 0) {
      ix += HISTORY_LENGTH;
    }

    resp += "<tr>\r\n";

    if (historyFan[ix]) {
      resp += "<td>1</td>\r\n";
    } else {
      resp += "<td>0</td>\r\n";
    }

    sprintf(temp, "<td>%1.0d</td>\r\n", (int) historyTemperature[ix]);
    resp += temp;
    sprintf(temp, "<td>%1.0d</td>\r\n", (int) historyHumidity[ix]);
    resp += temp;

    resp += "</tr>\r\n";
  }

  resp += "</table>\r\n";
  resp += "</body>\r\n";
  resp += "</html>\r\n";

  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "text/html", resp);
}

void serverStatus() {
  char temp[50];

  String resp = "{\r\n";

  resp += "\"mode\":";
  if (manualOverride) {
    resp += "\"manual\"";
  } else {
    resp += "\"auto\"";
  }
  resp += ",\r\n";

  resp += "\"fanRunning\":";
  if (fanRunning) {
    resp += "true";
  } else {
    resp += "false";
  }
  resp += ",\r\n";

  resp += "\"dhtError\":";
  if (dhtError) {
    resp += "true";
  } else {
    resp += "false";
  }
  resp += ",\r\n";

  sprintf(temp, "\"temperature\":%1.0d\r\n", (int) temperature);
  resp += temp;
  sprintf(temp, "\"humidity\":%1.0d\r\n", (int) humidity);
  resp += temp;

  resp += "}\r\n";

  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "application/json", resp);
}

void serverGraph() {
  int width = 1200;
  int height = 400;

  char temp[100];
  int pointWidth = width / (HISTORY_LENGTH - 1);

  String resp = "";
  sprintf(temp, "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"%d\" height=\"%d\">\r\n", width, height);
  resp += temp;
  sprintf(temp, "<rect width=\"%d\" height=\"%d\" stroke-width=\"1\" fill=\"#fff\" stroke=\"#fafafa\" />\r\n", width, height);
  resp += temp;

  // Grid lines
  resp += "<g stroke=\"#fafafa\">\r\n";

  for (int i = 0; i < 10; i++) {
    int y = (height / 10) * i;
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\r\n", 0, y, width, y);
    resp += temp;
  }

  resp += "</g>\r\n";

  // Humidity
  resp += "<g stroke=\"#98fb98\">\r\n";

  int lastY;
  for (int i = 1; i < HISTORY_LENGTH; i++) {
    int ix = historyPointer - i;
    if (ix < 0) {
      ix += HISTORY_LENGTH;
    }

    int x = (i - 2) * pointWidth;
    int y = height - ((int) historyHumidity[ix]) * (height / 100);

    if (i > 1) {
      sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\r\n", x, lastY, x + pointWidth, y);
      resp += temp;
    }

    lastY = y;
  }

  resp += "</g>\r\n";

  // Temperature
  resp += "<g stroke=\"#6495ed\">\r\n";

  for (int i = 1; i < HISTORY_LENGTH; i++) {
    int ix = historyPointer - i;
    if (ix < 0) {
      ix += HISTORY_LENGTH;
    }

    int x = (i - 2) * pointWidth;
    int y = height - ((int) historyTemperature[ix]) * (height / 100);

    if (i > 1) {
      sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\r\n", x, lastY, x + pointWidth, y);
      resp += temp;
    }

    lastY = y;
  }
  resp += "</g>\r\n";

  // Fan
  resp += "<g stroke=\"#f08080\">\r\n";

  resp += "<polyline fill=\"none\" stroke=\"#f08080\" stroke-width=\"1\" points=\"\r\n";

  for (int i = 1; i < HISTORY_LENGTH; i++) {
    int ix = historyPointer - i;
    if (ix < 0) {
      ix += HISTORY_LENGTH;
    }

    int x = (i - 1) * pointWidth;
    int y = historyFan[ix] ? 0 : height;

    sprintf(temp, "%d,%d\r\n", x, y);
    resp += temp;

    lastY = y;
  }
  resp += "\"/>\r\n";

  resp += "</g>\r\n";
  resp += "</svg>\r\n";

  server.send(200, "image/svg+xml", resp);
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
  server.on("/graph.svg", serverGraph);
  server.on("/manual/start", serverManualStart);
  server.on("/manual/cancel", serverManualCancel);
  server.on("/status", serverStatus);
  server.begin();
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
  pinMode(BUILTIN_LED, INPUT); // LED off - if it's set to an output it will be on
  pinMode(FAN_PIN, OUTPUT);

  // Connect to wifi
  WiFi.hostname("bathroom-fan");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Waiting for wifi...");

  while (WiFi.status() != WL_CONNECTED) {
    pinMode(BUILTIN_LED, INPUT);
    delay(200);
    pinMode(BUILTIN_LED, OUTPUT);
    delay(200);
    Serial.print(".");
  }

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Start
  serverStart();
  otaStart();
}

void loop() {
  // DHT
  if ((unsigned long)(millis() - dhtLastUpdateTime) > DHT_INTERVAL) {
    dhtRead();
    dhtLastUpdateTime = millis();
  }

  // Control
  if ((unsigned long)(millis() - controlLastUpdateTime) > CONTROL_INTERVAL) {
    controlLoop();
    controlLastUpdateTime = millis();
  }

  // History
  if ((unsigned long)(millis() - historyLastUpdateTime) > HISTORY_INTERVAL) {
    historyLoop();
    historyLastUpdateTime = millis();
  }

  server.handleClient();
  ArduinoOTA.handle();
  delay(100);
}
