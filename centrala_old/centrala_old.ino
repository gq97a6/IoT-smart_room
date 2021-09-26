//-------------------------------------------------------------------------------- Variables
#include <constants.h>
#define ADDRESS "cen"

#define STRIP_LEN_LA 2
#define STRIP_LEN_LB 76

#define STRIP_LEN_PA 6
#define STRIP_LEN_PB 72

#define STRIP_PIN_L 5
#define STRIP_PIN_P 18
#define STRIP_PIN_I 13

#define SUPPLY_5_PIN 15
#define SUPPLY_12_PIN 4
#define POTEN_PIN 34

//Delayed command execution
long DCETimers[MAX_DCE_TIMERS][2]; //When, distance
int DCELoop[MAX_DCE_TIMERS]; //-1 == retain, >0 == loop
String DCECommand[MAX_DCE_TIMERS]; //Command

//-------------------------------------------------------------------------------- Libraries
//Wifi
#include <WiFi.h>

//OTA
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

//UDP
#include "AsyncUDP.h"

//MQTT
#include <PubSubClient.h>

//EEPROM
#include <Preferences.h>

//Buttons
#include <IoT_Modules-Buttons.h>

//LED strips
#include <FastLED.h>

//BME280 sensor
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
Adafruit_BME280 bme;

//-------------------------------------------------------------------------------- Objects
WiFiClient wifiClient;
PubSubClient client(wifiClient);
AsyncUDP udp;

CRGB stripLA[STRIP_LEN_LA];
CRGB stripLB[STRIP_LEN_LB];

CRGB stripPA[STRIP_LEN_PA];
CRGB stripPB[STRIP_LEN_PB];

CRGB stripI[3];

Preferences preferences;

ModulesButton buttonsHolder[5];
ModulesButtonsCollection b(buttonsHolder, 5);

//-------------------------------------------------------------------------------- Status
char charArray[8];
uint8_t gHue;
long wifiReconAlarm;
long mqttReconAlarm;
long buttonCheckAlarm;
byte anim;
bool c5;
bool c12;
bool topBar;
byte potenPos;
bool valve;
String queryS;
String lastStripColor;

void setup()
{
  Serial.begin(115200);

  b.add(27, "red");
  b.add(26, "green");
  b.add(25, "blue");
  b.add(33, "yellow");
  b.add(32, "black");

  b.setPinMode(INPUT_PULLUP);
  b.setSleepInterval(500);
  b.invert(true);

  pinMode(POTEN_PIN, INPUT); //Poten
  pinMode(SUPPLY_5_PIN, OUTPUT); //5VDC supply
  pinMode(SUPPLY_12_PIN, OUTPUT); //12VDC supply

  //Power supply
  digitalWrite(SUPPLY_5_PIN, HIGH);
  digitalWrite(SUPPLY_12_PIN, HIGH);

  FastLED.addLeds<WS2812B, STRIP_PIN_L, GRB>(stripLA, STRIP_LEN_LA).setCorrection(CRGB(255, 255, 255));
  FastLED.addLeds<WS2812B, STRIP_PIN_L, GRB>(stripLB, STRIP_LEN_LB).setCorrection(CRGB(255, 255, 255));
  FastLED.addLeds<WS2812B, STRIP_PIN_P, GRB>(stripPA, STRIP_LEN_PA).setCorrection(CRGB(255, 255, 255));
  FastLED.addLeds<WS2812B, STRIP_PIN_P, GRB>(stripPB, STRIP_LEN_PB).setCorrection(CRGB(255, 255, 255));
  FastLED.addLeds<WS2812B, STRIP_PIN_I, GRB>(stripI, 3).setCorrection(CRGB(255, 179, 166));
  FastLED.setBrightness(255);

  //MQTT
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(mqttCallback);

  //Wifi and OTA update
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  String host_name_String = "node_" + String(ADDRESS);
  if (host_name_String.length() < 15)
  {
    char host_name[15];
    host_name_String.toCharArray(host_name, 15);
    WiFi.setHostname(host_name);
  }

  delay(1000);

  ArduinoOTA
  .onStart([]()
  {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
  })
  .onEnd([]()
  {
    Serial.println("\nEnd");
  })
  .onProgress([](unsigned int progress, unsigned int total)
  {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error)
  {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");

    digitalWrite(2, HIGH);
    delay(3000);
    digitalWrite(2, LOW);

    ESP.restart();
  });

  if (udp.listen(UDP_PORT))
  {
    udp.onPacket([](AsyncUDPPacket packet)
    {
      //Extract adress
      String adr = "";
      for (int i = 0; i < packet.length(); i++)
      {
        adr += (char)packet.data()[i];
        if ((char)packet.data()[i + 1] == ';')
        {
          break;
        }
      }

      //Execute command
      if (adr == ADDRESS || adr == "glb")
      {
        String cmd = "";
        for (int i = 4; i < packet.length(); i++)
        {
          cmd += (char)packet.data()[i];
        }
        terminal(cmd);
      }
    });
  }

  ArduinoOTA.begin();
  bme.begin();

  DCEAdd(UPDATE_FREQ, "upair;com", -1);

  //Turn of after reset
  terminal("bout;7;#000000");

  //Set white color as default
  terminal("cwip;#FFFFFF");
}

void loop()
{
  conErrorHandle(); //OTA and connection maintenance
  b.handle();
  potenHandle();
  DCEHandle();

  //RED / Heating control
  if (b.isOnly("red", HIGH) && b.get("red").ascending) {
    switch (potenPos)
    {
      case 0:
        terminal("sendUDP;loz;heat;0");
        terminal("bout;1;#0000FF");
        DCEAdd(1000, "bout;7;#000000", 1);
        break;

      case 2:
        terminal("sendUDP;loz;heat;1");
        terminal("bout;2;#FF0000");
        DCEAdd(1000, "bout;7;#000000", 1);
        break;

      case 4:
        terminal("sendUDP;loz;heat;4");

        if (valve)
        {
          terminal("bout;4;#0000FF");
          DCEAdd(1000, "bout;7;#000000", 1);
        }
        else
        {
          terminal("bout;4;#FF0000");
          DCEAdd(1000, "bout;7;#000000", 1);
        }
        break;
    }
  }

  //GREEN
  if (b.isOnly("green", HIGH) && b.get("green").ascending)
  {
    terminal("bout;7;#00FF00");
    DCEAdd(1000, "bout;7;#000000", 1);
    //terminal("sendUDP; loz; shw; 0");0000", 1);

    c5 = !c5;

    if (c5) {
      digitalWrite(SUPPLY_5_PIN, LOW);
      terminal("cwip;" + lastStripColor);
      FastLED.show();
    } else {
      terminal("cwip;#000000;1");
      FastLED.show();
      digitalWrite(SUPPLY_5_PIN, HIGH);
    }
  }

  //BLUE / Fan control //0(gear 0) 1-160(gear 1) 161-175(X) 176-335(gear 2) 336-350(X) 351-511(gear 3)
  if (b.isOnly("blue", HIGH) && b.get("blue").ascending) {
    terminal("sendUDP;wen;fan;-1");

    //    terminal("bout;1;#0000FF");
    //    DCEAdd(500, "bout;7;#000000", 1);
    //
    //    DCEAdd(500, "bout;2;#0000FF", 1);
    //    DCEAdd(1000, "bout;7;#000000", 1);
    //
    //    DCEAdd(1000, "bout;4;#0000FF", 1);
    //    DCEAdd(1500, "bout;7;#000000", 1);
  }
  
  //YELLOW / Top bar controlw
  if (b.isOnly("yellow", HIGH) && b.get("yellow").ascending)
  {
    if (topBar)
    {
      topBar = 0;
      c12 = 0;
      digitalWrite(SUPPLY_12_PIN, HIGH);
    }
    else
    {
      topBar = 1;
      c12 = 1;
      digitalWrite(SUPPLY_12_PIN, LOW);
    }
  }

  //BLACK / Blackout
  if (b.isOnly("black", HIGH) && b.get("black").ascending)
  {
    client.publish("loz5", "0");
    client.publish("wenfan", "0");

    terminal("sendUDP;loz;5;0");
    terminal("sendUDP;wen;fan;0");

    c5 = 0;
    digitalWrite(SUPPLY_12_PIN, HIGH);

    topBar = 0;
    c12 = 0;
    digitalWrite(SUPPLY_5_PIN, HIGH);
  }

  //ALL / Restart
  if (b.areAll(HIGH)) {
    terminal("reset");
  }

  //Led strips
//  Fire2012();
//  rainbow();
//  confetti();
//  sinelon();
//  bpm();

  delay(1000 / FRAMES_PER_SECOND);
  EVERY_N_MILLISECONDS(20)
  {
    gHue++;
  }
}

void conErrorHandle()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    ArduinoOTA.handle();

    //    if (!client.loop()) //No connection with MQTT server
    //    {
    //      Serial.println("Disconnected from MQTT server!");
    //      if (millis() >= mqttReconAlarm)
    //      {
    //        Serial.println("Reconnecting to MQTT server.");
    //        mqttReconAlarm = millis() + MQTT_RECON_FREQ;
    //        mqttReconnect();
    //      }
    //    }
  }
  else //No connection with wifi router
  {
    Serial.println("Disconnected from wifi router!");
    if (millis() >= wifiReconAlarm)
    {
      Serial.println("Reconnecting to wifi router.");
      wifiReconAlarm = millis() + WIFI_RECON_FREQ;

      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }
}

void mqttReconnect()
{
  //Create a random client ID
  String clientId = "ESP32#";
  clientId += ADDRESS;

  // Attempt to connect
  if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD))
  {
    //Subscribe list
    client.subscribe("#");
  }
}

void DCEHandle()
{
  for (int i = 0; i < MAX_DCE_TIMERS; i++)
  {
    if (millis() >= DCETimers[i][0] && DCELoop[i] != 0)
    {
      //Execute command
      terminal(DCECommand[i]);

      if (DCELoop[i] == -1) // Infinite, extend
      {
        DCETimers[i][0] = millis() + DCETimers[i][1];
      }
      else if (DCELoop[i] > 1) //X times, extend, decrease
      {
        DCETimers[i][0] = millis() + DCETimers[i][1];
        DCELoop[i] -= 1;
      }
      else if (DCELoop[i] == 1) //Close DCE, its last iteration
      {
        DCELoop[i] = 0;
      }
    }
  }
}

void DCEAdd(long timer, String command, int loops)
{
  for (int i = 0; i < MAX_DCE_TIMERS; i++)
  {
    if (DCELoop[i] == 0) //Look for first empty
    {
      //Command
      DCETimers[i][0] = timer + millis();
      DCETimers[i][1] = timer;
      DCELoop[i] = loops;
      DCECommand[i] = command;

      return;
    }
  }
}

bool DCEEdit(long timer, String command, int loops)
{
  bool edited = 0;

  for (int i = 0; i < MAX_DCE_TIMERS; i++)
  {
    if (DCECommand[i] == command || DCELoop[i] != 0) //Look for desired command and edit
    {
      edited = 1;

      //Command
      if (timer > 0)
      {
        DCETimers[i][0] = timer + millis();
        DCETimers[i][1] = timer;
      }
      else if (timer == 0)
      {
        DCETimers[i][0] = 0;
      }

      DCECommand[i] = command;
      DCELoop[i] = loops;
    }
  }

  return edited;
}

byte potenHandle()
{
  int poten = analogRead(POTEN_PIN);
  potenPos = -1;

  if (poten == 0)
  {
    potenPos = 0;
  }
  else if (poten <= 930)
  {
    potenPos = 1;
  }
  else if (poten <= 2350)
  {
    potenPos = 2;
  }
  else if (poten < 4095)
  {
    potenPos = 3;
  }
  else if (poten == 4095)
  {
    potenPos = 4;
  }

  return potenPos;
}

//-------------------------------------------------------------------------------- Strip functions
//void colorWipe(uint32_t color, int wait, int first, int last)
//{
//  for (int i = first; i <= last; i++)
//  {
//    stripL[i] = color;
//    stripP[i] = color;
//    FastLED.show();
//
//    delay(wait);
//  }
//}
//
//#define COOLING  55
//#define SPARKING 120
//#define NUM_LEDS  156
//
//void Fire2012()
//{
//  if (anim == 5)
//  {
//    // Array of temperature readings at each simulation cell
//    static byte heat[NUM_LEDS];
//
//    // Step 1. Cool down every cell a little
//    for (int i = 0; i < NUM_LEDS; i++)
//    {
//      heat[i] = qsub8(heat[i], random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
//    }
//
//    // Step 2. Heat from each cell drifts 'up' and diffuses a little
//    for (int k = NUM_LEDS - 1; k >= 2; k--)
//    {
//      heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
//    }
//
//    // Step 3. Randomly ignite new 'sparks' of heat near the bottom
//    if (random8() < SPARKING)
//    {
//      int y = random8(7);
//      heat[y] = qadd8(heat[y], random8(160, 255));
//    }
//
//    // Step 4. Map from heat cells to LED colors
//    for (int j = 0; j < NUM_LEDS; j++)
//    {
//      CRGB color = HeatColor(heat[j]);
//
//      if (j < 78)
//      {
//        stripP[77 - j] = color;
//      }
//      else
//      {
//        stripL[j - 78] = color;
//      }
//    }
//
//    FastLED.show();
//  }
//}
//
//void rainbow()
//{
//  if (anim == 1)
//  {
//    fill_rainbow(stripL, 78, gHue, 7);
//    fill_rainbow(stripP, 78, gHue, 7);
//
//    FastLED.show();
//  }
//}
//
//void confetti()
//{
//  if (anim == 2)
//  {
//    fadeToBlackBy(stripL, 78, 10);
//    fadeToBlackBy(stripP, 78, 10);
//    int pos = random16(78);
//    stripP[pos] += CHSV(gHue + random8(64), 200, 255);
//    stripL[pos] = stripP[pos];
//
//    FastLED.show();
//  }
//}
//
//void sinelon()
//{
//  if (anim == 3)
//  {
//    fadeToBlackBy(stripP, 78, 20);
//    fadeToBlackBy(stripL, 78, 20);
//
//    int pos = beatsin16(13, 0, 78 - 1);
//    stripP[pos] += CHSV(gHue, 255, 192);
//    stripL[pos] = stripP[pos];
//
//    FastLED.show();
//  }
//}
//
//void bpm()
//{
//  if (anim == 4)
//  {
//    uint8_t BeatsPerMinute = 62;
//    uint8_t beat = beatsin8(BeatsPerMinute, 64, 255);
//
//    CRGBPalette16 palette = PartyColors_p;
//    for (int i = 0; i < 78; i++) //9948
//    {
//      stripP[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
//      stripL[i] = stripP[i];
//    }
//
//    FastLED.show();
//  }
//}

int toIntColor(char hexColor[])
{
  int base = 1;
  int color = 0;

  for (int i = 6; i >= 1; i--)
  {
    if (hexColor[i] >= '0' && hexColor[i] <= '9')
    {
      color += (hexColor[i] - 48) * base;
      base = base * 16;
    }
    else if (hexColor[i] >= 'A' && hexColor[i] <= 'F')
    {
      color += (hexColor[i] - 55) * base;
      base = base * 16;
    }
  }

  return color;
}

unsigned int hexToDec(String hexString) {
  unsigned int decValue = 0;
  int nextInt;

  for (int i = 0; i < hexString.length(); i++) {

    nextInt = int(hexString.charAt(i));
    if (nextInt >= 48 && nextInt <= 57) nextInt = map(nextInt, 48, 57, 0, 9);
    if (nextInt >= 65 && nextInt <= 70) nextInt = map(nextInt, 65, 70, 10, 15);
    if (nextInt >= 97 && nextInt <= 102) nextInt = map(nextInt, 97, 102, 10, 15);
    nextInt = constrain(nextInt, 0, 15);

    decValue = (decValue * 16) + nextInt;
  }

  return decValue;
}

String decToHex(byte decValue, byte desiredStringLength) {

  String hexString = String(decValue, HEX);
  while (hexString.length() < desiredStringLength) hexString = "0" + hexString;

  return hexString;
}

//-------------------------------------------------------------------------------- Callbacks
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  //Get payload
  String payloadS;
  for (int i = 0; i < length; i++)
  {
    payloadS += (char)payload[i];
  }

  if (String(topic) == "terminal") //Terminal input, payload is a command
  {
    String adr = String(payloadS).substring(0, String(payloadS).indexOf(';'));
    if (adr == ADDRESS || adr == "glb")
    {
      terminal(String(payloadS).substring(String(payloadS).indexOf(';') + 1));
    }
  }
  else //Standard input, topic is a command, payload is a variable
  {
    String adr = String(topic).substring(0, String(topic).indexOf(';'));
    if (adr == ADDRESS || adr == "glb")
    {
      terminal(String(topic).substring(String(topic).indexOf(';') + 1) + ";" + payloadS);
    }
  }
}

void terminal(String command)
{
  String cmd[MAXT_ELEMENTS];
  terminalSlice(command, cmd);

  if (cmd[0] == "cwip")
  {
    if(cmd[2] != "1") lastStripColor = cmd[1];
    
    cmd[1].toUpperCase();
    cmd[1].toCharArray(charArray, 8);
    int color = toIntColor(charArray);

    for (int i = 0; i < STRIP_LEN_LA; i++)
    {
      stripLA[i] = color;
    }

    for (int i = 0; i < STRIP_LEN_LB; i++)
    {
      stripLB[i] = color;
    }

    for (int i = 0; i < STRIP_LEN_PA; i++)
    {
      stripPA[i] = color;
    }

    for (int i = 0; i < STRIP_LEN_PB; i++)
    {
      stripPB[i] = color;
    }

    FastLED.show();
    FastLED.show();
  }
  //-------------------------------------------------------------------------------- Set color of one diode
  else if (cmd[0] == "setd")
  { //TODO
//    cmd[1].toUpperCase();
//    cmd[1].toCharArray(charArray, 8);
//    int colorA = toIntColor(charArray);
//
//    cmd[1] = cmd[1].substring(0, 3) + decToHex(hexToDec(cmd[1].substring(3, 5)) * 70 / 100, 2) + decToHex(hexToDec(cmd[1].substring(5)) * 65 / 100, 2);
//    cmd[1].toUpperCase();
//    cmd[1].toCharArray(charArray, 8);
//    int colorB = toIntColor(charArray);
//
//    if (cmd[2].toInt() < 2)
//    {
//      stripL[cmd[2].toInt()] = colorA;
//      stripP[cmd[2].toInt()] = colorA;
//    }
//    else if (cmd[2].toInt() < 6)
//    {
//      stripL[cmd[2].toInt()] = colorB;
//      stripP[cmd[2].toInt()] = colorA;
//    }
//    else
//    {
//      stripL[cmd[2].toInt()] = colorB;
//      stripP[cmd[2].toInt()] = colorB;
//    }
//
//    FastLED.show();
  }
  else if (cmd[0] == "infd")
  {
    cmd[1].toCharArray(charArray, 8);
    int color = toIntColor(charArray);
    stripI[cmd[2].toInt()] = color;

    FastLED.show();
  }
  //--------------------------------------------------------------------------------
  else if (cmd[0] == "reset")
  {
    terminal("bout;7;#FF0000");
    delay(500);
    ESP.restart();
  }
  //--------------------------------------------------------------------------------
  else if (cmd[0] == "anim")
  {
    anim = cmd[1].toInt();
  }
  //-------------------------------------------------------------------------------- 5V power supply
  else if (cmd[0] == "5")
  {
    if (cmd[1] == "1")
    {
      digitalWrite(SUPPLY_5_PIN, LOW);
    }
    else if (cmd[1] == "0")
    {
      digitalWrite(SUPPLY_5_PIN, HIGH);
    }
  }
  //-------------------------------------------------------------------------------- 12V power supply
  else if (cmd[0] == "12")
  {
    if (cmd[1] == "1")
    {
      digitalWrite(SUPPLY_12_PIN, LOW);
    }
    else if (cmd[1] == "0")
    {
      digitalWrite(SUPPLY_12_PIN, HIGH);
    }
  }
  else if (cmd[0] == "bip")
  {
    ledcWrite(0, cmd[1].toInt());
  }
  else if (cmd[0] == "botim")
  {
    if (cmd[1].toInt() == 0)
    {
      //Turn off
      DCEEdit(0, "black", 0);
    }
    else
    {
      long timer = cmd[1].toInt() * 60 * 1000; //Minutes to milliseconds

      DCEAdd(timer, "black", 1);
      DCEAdd(timer, "sendUDP;wen;fan;0", 1);
      DCEAdd(timer, "sendUDP;loz;5;0", 1);
      DCEAdd(timer, "sendUDP;loz;anim;0", 1);
    }
  }
  else if (cmd[0] == "black")
  {
    terminal("12;0");
    terminal("5;0");
    terminal("anim;0");
  }
  else if (cmd[0] == "vlv")
  {
    valve = cmd[1].toInt();
  }
  else if (cmd[0] == "upair")
  {
    float temp = bme.readTemperature();
    int humi = bme.readHumidity();
    int pres = bme.readPressure() / 100.0F;

    terminal("sendUDP;" + String(cmd[1]) + ";air;" + String(temp) + ';' + String(humi) + ';' + String(pres));

    String(temp).toCharArray(charArray, 8);
    client.publish("glb;temp", charArray);

    String(humi).toCharArray(charArray, 8);
    client.publish("glb;humi", charArray);

    String(pres).toCharArray(charArray, 8);
    client.publish("glb;pres", charArray);
  }
  else if (cmd[0] == "giveAir") { //TEST
    float temp = bme.readTemperature();
    int humi = bme.readHumidity();
    int pres = bme.readPressure() / 100.0F;

    terminal("sendUDP;pho;air;" + String(temp) + ';' + String(humi) + ';' + String(pres));
  }
  else if (cmd[0] == "bout")
  {
    cmd[1] = String(cmd[1].toInt(), BIN);
    cmd[2].toCharArray(charArray, 8);
    int color = toIntColor(charArray);

    //Read array backward, input colors into array form the front
    for (int i = cmd[1].length() - 1; i >= 0; i--)
    {
      if (cmd[1].substring(i, i + 1).toInt())
      {
        stripI[cmd[1].length() - 1 - i] = color;
      }
    }

    FastLED.show();
  }
  else if (cmd[0] == "sendUDP") //address, command, A, B, C...
  {
    String toSend = cmd[1] + ';' + cmd[2]; //Address and command

    //Parameters
    for (int i = 3; i < MAXT_CMD; i++)
    {
      if (cmd[i] != "")
      {
        toSend += ';';
        toSend += cmd[i];
      }
      else
      {
        toSend += ";|";
        break;
      }
    }

    byte len = toSend.length();
    if (len < MAXT_CMD)
    {
      byte toSendA[len + 2];
      toSend.getBytes(toSendA, len + 2);

      if (cmd[1] == "glb")
      {
        udp.writeTo(toSendA, len, CEN_ADR, UDP_PORT);
        udp.writeTo(toSendA, len, LOZ_ADR, UDP_PORT);
        udp.writeTo(toSendA, len, WEN_ADR, UDP_PORT);
      }
      else if (cmd[1] == "cen") {
        udp.writeTo(toSendA, len, CEN_ADR, UDP_PORT);
      }
      else if (cmd[1] == "loz") {
        udp.writeTo(toSendA, len, LOZ_ADR, UDP_PORT);
      }
      else if (cmd[1] == "wen") {
        udp.writeTo(toSendA, len, WEN_ADR, UDP_PORT);
      }
      else if (cmd[1] == "com") {
        udp.writeTo(toSendA, len, COM_ADR, UDP_PORT);
      }
      else if (cmd[1] == "pho") {
        IPAddress phoneADR(192, 168, 0, 16);
        udp.writeTo(toSendA, len, phoneADR, UDP_PORT);
      }
    }
  }
}
