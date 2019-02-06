#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <VitoWiFi.h>
#include <AsyncMqttClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>

//HTTP-Server fuer Info-Webseite
ESP8266WebServer httpServer(80);
//HTTP-Server fuer Sketch/Firmware Update
ESP8266HTTPUpdateServer httpUpdater;

//###### Variablen
volatile bool updateVitoWiFi = false;
bool bStopVito = false;
Ticker timer;

//###### Konfiguration
static const char SSID[] = "TDHome";
static const char PASS[] = ".!DianaVogel.!";
static const IPAddress BROKER(192, 168, 10, 60);
static const uint16_t PORT =  1883;
static const char CLIENTID[] = "VitoWifi";
static const char MQTTUSER[] = "hamqtt";
static const char MQTTPASS[] = "H72ix4";
static const int READINTERVAL = 60; //Abfrageintervall OptoLink, in Sekunden
VitoWiFi_setProtocol(P300);

//###### Allgemein
DPTemp getTempA("gettempa", "allgemein", 0x0800);               //Aussentemperatur
DPStat getAlarmStatus("getalarmstatus", "allgemein", 0x0A82);   //Sammelstoerung Ja/Nein

//###### Warmwasser
DPTemp getTempWWist("gettempwwist", "wasser", 0x0804);          //Warmwasser-Ist
DPTempS getTempWWsoll("gettempwwsoll", "wasser", 0x6300);       //Warmwasser-Soll
//DPTempS setTempWWsoll("settempwwsoll", "wasser", 0x6300);     //Warmwasser-Soll schreiben

//###### Kessel
DPTemp getTempKist("gettempkist", "kessel", 0x0802);            //Kesseltemperatur-Ist

//###### Brenner
DPStat getBrennerStatus("getbrennerstatus", "brenner", 0x55D3);       //Brennerstatus
DPHours getBrennerStunden1("getbrennerstunden1", "brenner", 0x08A7);  //Brennerstunden Stufe1

//###### Heizkreise
DPTemp getTempVListM1("gettempvlistm1", "heizkreise", 0x2900);                  //HK1 Vorlauftemp
DPTemp getTempVListM2("gettempvlistm2", "heizkreise", 0x3900);                  //HK2 Vorlauftemp
DPTempS getTempRaumNorSollM1("gettempraumnorsollm1", "heizkreise", 0x2306);     //HK1 Raumtemp-Soll
//DPTempS setTempRaumNorSollM1("settempraumnorsollm1", "heizkreise", 0x2306);   //HK1 Raumtemp-Soll schreiben
DPTempS getTempRaumNorSollM2("gettempraumnorsollm2", "heizkreise", 0x3306);     //HK2 Raumtemp-Soll
//DPTempS setTempRaumNorSollM2("settempraumnorsollm2", "heizkreise", 0x3306);   //HK2 Raumtemp-Soll schreiben

//###### Betriebsarten
DPMode getBetriebArtM1("getbetriebartm1","betriebsarten", 0x2301);     //HK1 0=Abschaltb,1=nur WW,2=heiz+WW, 3=DauernRed,3=Dauer Norma.
DPMode getBetriebArtM2("getbetriebartm2","betriebsarten", 0x3301);     //HK2 0=Abschaltb,1=nur WW,2=heiz+WW, 3=DauernRed,3=Dauer Norma.
DPStat getBetriebPartyM1("getbetriebpartym1","betriebsarten", 0x2303); //HK1 Party
DPStat getBetriebPartyM2("getbetriebpartym2","betriebsarten", 0x3303); //HK2 Party
DPStat setBetriebPartyM1("setbetriebpartym1","betriebsarten", 0x2330); //HK1 Party schreiben
DPStat setBetriebPartyM2("setbetriebpartym2","betriebsarten", 0x3330); //HK2 Party schreiben


//###### Objekte und Event-Handler
AsyncMqttClient mqttClient;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;


void connectToWiFi() {
  //Mit WLAN verbinden
  WiFi.begin(SSID, PASS);
}

void connectToMqtt() {
  //Mit MQTT-Server verbinden
  mqttClient.connect();
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  //Wenn WLAN-Verbindung steht und IP gesetzt, zu MQTT verbinden
  timer.once(2, connectToMqtt);
}

void onMqttConnect(bool sessionPresent) {
  //Wenn MQTT verbunden, Topics abonnieren
  mqttClient.subscribe("VITOWIFI/setBetriebPartyM1", 0);
  mqttClient.subscribe("VITOWIFI/setBetriebPartyM2", 0);
  //mqttClient.subscribe("VITOWIFI/setTempWWsoll", 0);
  //mqttClient.subscribe("VITOWIFI/setTempRaumNorSollM1", 0);
  //mqttClient.subscribe("VITOWIFI/setTempRaumNorSollM2", 0);
  mqttClient.publish("VITOWIFI/$status/online", 1, true, "1");  //Topic setzen, dieser haelt den Verfuegbarkeitsstatus
  //Timer aktivieren, alle X Sekunden die Optolink-Schnittstelle abfragen
  timer.attach(READINTERVAL, [](){
    updateVitoWiFi = true;
  });
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  //Wenn Mqtt Verbindung verloren und wifi noch verbunden
  if (WiFi.isConnected()) {
    //Mqtt erneut verbinden
    timer.once(2, connectToMqtt);
  }
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  //Wenn WLAN Verbindung verloren, neu verbinden
  timer.once(2, connectToWiFi);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  //Wenn abonnierte MQTT Nachricht erhalten
  if(strcmp(topic,"VITOWIFI/setBetriebPartyM1") == 0) {
     bool setParty = 0;
     //Wert(Payload) auswerten und Variable setzen
     if(strcmp(payload,"1") == 0) setParty = 1;
     //In DPValue Konvertieren (siehe github vitowifi fuer Datentypen)
     DPValue value(setParty);
     //Wert an Optolink schicken
     VitoWiFi.writeDatapoint(setBetriebPartyM1, value);
     //Wert auslesen um aktuellen Status an MQTT-Broker zu senden
     VitoWiFi.readDatapoint(getBetriebPartyM1);
  }
  if(strcmp(topic,"VITOWIFI/setBetriebPartyM2") == 0) {
     bool setParty = 0;
     if(strcmp(payload,"1") == 0) setParty = 1;
     DPValue value(setParty);
     VitoWiFi.writeDatapoint(setBetriebPartyM2, value);
     VitoWiFi.readDatapoint(getBetriebPartyM2);
  }
  /*if(strcmp(topic,"VITOWIFI/setTempWWsoll") == 0) {
     uint8_t setTemp = atoi(payload);
     if(setTemp>=45 && setTemp<=60){
       DPValue value(setTemp);
       VitoWiFi.writeDatapoint(setTempWWsoll, value);
       VitoWiFi.readDatapoint(getTempWWsoll);
     }
  }
  if(strcmp(topic,"VITOWIFI/setTempRaumNorSollM1") == 0) {
     uint8_t setTemp = atoi(payload);
     if(setTemp>=3 && setTemp<=37){
       DPValue value(setTemp);
       VitoWiFi.writeDatapoint(setTempRaumNorSollM1, value);
       VitoWiFi.readDatapoint(getTempRaumNorSollM1);
     }
  }
  if(strcmp(topic,"VITOWIFI/setTempRaumNorSollM2") == 0) {
     uint8_t setTemp = atoi(payload);
     if(setTemp>=3 && setTemp<=37){
       DPValue value(setTemp);
       VitoWiFi.writeDatapoint(setTempRaumNorSollM2, value);
       VitoWiFi.readDatapoint(getTempRaumNorSollM2);
     }
  }*/
}

//DPTemp & DPHours - float |  MQTT-Topic bsp: VITOWIFI/gettempa 
void tempCallbackHandler(const IDatapoint& dp, DPValue value) {
  //Umwandeln, und zum schluss per mqtt publish an mqtt-broker senden
  char outVal[9];
  dtostrf(value.getFloat(), 6, 2, outVal);
  char outName[30] = "VITOWIFI/";
  strcpy(outName,dp.getName());
  mqttClient.publish(outName, 1, true, outVal);  
}

//DPTemps - uint8_t |  MQTT-Topic bsp: VITOWIFI/getBetriebArtM1
void tempSCallbackHandler(const IDatapoint& dp, DPValue value) {
  //Umwandeln, und zum schluss per mqtt publish an mqtt-broker senden
  int nValue = value.getU8();
  char outName[30] = "VITOWIFI/";
  strcpy(outName,dp.getName());
  mqttClient.publish(outName, 1, true, String(nValue).c_str()); 
}

//DPStat - bool |  MQTT-Topic bsp: VITOWIFI/getbrennerstatus 
void statCallbackHandler(const IDatapoint& dp, DPValue value) {
  //Umwandeln, und zum schluss per mqtt publish an mqtt-broker senden
  char outName[30] = "VITOWIFI/";
  strcpy(outName,dp.getName());
  mqttClient.publish(outName, 1, true, (value.getBool()) ? "1" : "0");  
}

void setup() {
  //DEBUG WiFi.mode(WIFI_AP_STA);
  //Setze WLAN-Optionen
  WiFi.mode(WIFI_STA);
  WiFi.hostname(CLIENTID);

  //Setze Datenpunkte als beschreibbar
  setBetriebPartyM1.setWriteable(true);
  setBetriebPartyM2.setWriteable(true);
  //setTempWWsoll.setWriteable(true);
  //setTempRaumNorSollM1.setWriteable(true);
  //setTempRaumNorSollM2.setWriteable(true);

  //Zuweisung der Datenpunkte anhand des Rueckgabewerts an entsprechende Handler
  //(siehe github vitowifi fuer Datentypen)
  getTempA.setCallback(tempCallbackHandler);
  getTempWWist.setCallback(tempCallbackHandler);
  getTempKist.setCallback(tempCallbackHandler);
  getTempVListM1.setCallback(tempCallbackHandler);
  getTempVListM2.setCallback(tempCallbackHandler);
  getBrennerStunden1.setCallback(tempCallbackHandler);

  getTempWWsoll.setCallback(tempSCallbackHandler);
  getTempRaumNorSollM1.setCallback(tempSCallbackHandler);
  getTempRaumNorSollM2.setCallback(tempSCallbackHandler);

  getAlarmStatus.setCallback(statCallbackHandler);
  getBrennerStatus.setCallback(statCallbackHandler);
  getBetriebPartyM1.setCallback(statCallbackHandler);
  getBetriebPartyM2.setCallback(statCallbackHandler);
  
  //Wichtig, da sonst ueber die Serielle-Konsole (Optolink) Text geschrieben wird
  VitoWiFi.disableLogger();
  //Setze Serielle PINS an VitoWifi
  VitoWiFi.setup(&Serial);

  //Verbindungsaufbau und setzen der Optionen
  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
  mqttClient.setServer(BROKER, PORT);
  mqttClient.setClientId(CLIENTID);
  mqttClient.setCredentials(MQTTUSER, MQTTPASS);
  mqttClient.setKeepAlive(5);
  mqttClient.setCleanSession(true);
  mqttClient.setWill("VITOWIFI/$status/online", 1, true, "0");
  connectToWiFi();

  //Info-Webseite anzeigen auf HTTP-Port 80
  httpServer.on("/", [](){
    httpServer.send(200, "text/html", "<h1>Vito-Status: " + String((bStopVito ? "Stopped" : "Running")) + "</h1><a href='http://vitowifi.lan/start'>Start</a> <a href='http://vitowifi.lan/stop'>Stop</a> <br><br> <b>Compiled: " __DATE__ " " __TIME__ "</b><br><br><a href='http://vitowifi.lan/reboot'>reboot</a><br><a href='http://vitowifi.lan/update'>update</a><br>");
  });

  //Stop-Funktion sollte etwas schieflaufen :)
  httpServer.on("/stop", [](){
    bStopVito = true;
    httpServer.send(200, "text/plain", "OK - Stopped");
  });
  //Startfunktion
  httpServer.on("/start", [](){
    bStopVito = false;
    httpServer.send(200, "text/plain", "OK - Started");
  });
  //Reboot ueber Webinterface
  httpServer.on("/reboot", [](){
    httpServer.send(200, "text/plain", "OK - rebooting...");
    ESP.restart();
  });

  //Starte Webserver fuer Sketch/Firmware-Update
  httpUpdater.setup(&httpServer);
  httpServer.begin();
}

void loop() {
  if (!bStopVito){
    VitoWiFi.loop();
    if (updateVitoWiFi && mqttClient.connected()) {
      updateVitoWiFi = false;
      VitoWiFi.readAll();
    }
  }
  httpServer.handleClient();
}
