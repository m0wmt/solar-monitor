/*
  Simple monitor application with GUI to show current solar generation 
  and total so far today.  Subscribes to a MQTT topic which is sent via
  the broker on the Pi.  Will also show other information as we progress
  with other features!
*/
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h> 
#include <SPI.h>
#include <Button2.h>

#include "config.h"

#define BUTTON_1 35
#define BUTTON_2 0
#define DEBOUNCETIME  10

typedef enum {
  CURRENTPV,
  DAILYPV,
  INFO
} CURRENT_SCREEN;

TFT_eSPI tft = TFT_eSPI();
WiFiClient wifiClient;
PubSubClient client(wifiClient);

static portMUX_TYPE myMux = portMUX_INITIALIZER_UNLOCKED;
volatile CURRENT_SCREEN currentScreen = CURRENTPV;
volatile CURRENT_SCREEN newScreen = CURRENTPV;

// Hardware timer for updating clock etc.
hw_timer_t *My_timer = NULL;
volatile bool update = false;
char timeStringBuff[35]; //35 chars should be enough - buffer for time on the display

// Button sepcific
Button2 btn1(BUTTON_1);
Button2 btn2(BUTTON_2);
volatile bool lastState;
volatile uint32_t debounceTimeout = 0;
volatile int numberOfButtonInterrupts = 0;
uint32_t saveDebounceTimeout;
bool saveLastState;
int save;

// Define meter size
#define M_SIZE      1
#define TFT_GREY    0x5AEB
#define TFT_ORANGE  0xFBE1    // RGB 255 127 00
#define TFT_TEAL    0x028A    // RGB 00 80 80

float ltx = 0;    // Saved x coord of bottom of needle
uint16_t osx = M_SIZE*120, osy = M_SIZE*120; // Saved x & y coords

int old_analog =  -999; // Value last displayed
float old_analog2 =  -999.9; // Value last displayed

volatile int pvnow = 0;
volatile float dailypv = 0.0;

//int value[6] = {0, 0, 0, 0, 0, 0};
//int old_value[6] = { -1, -1, -1, -1, -1, -1};
//int d = 0;



// Attempt to reconnect to the MQTT broker
void reconnect(void) {
  // Loop until we're reconnected
  String clientId = "ESP32Client-";
  clientId += String(random(0xffff), HEX);
  boolean result = false;

  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_USER_PASSWORD)) {
      Serial.println("connected");
      client.setSocketTimeout(120);

      // subscribe to topics we're interested in
      if (client.subscribe("solar/pvnow", 0)) {
        Serial.println("Subscribed to solar/pvnow");
      };
      if (client.subscribe("solar/pvtotal", 0)) {
        Serial.println("Subscribed to solar/pvtotal");
      }
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void printLocalTime(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S %B %d %Y", &timeinfo);

  tft.setTextColor(TFT_GREY, TFT_BLACK);
  tft.drawRightString(timeStringBuff, 200, 5, 2);
}

// timer interrupt routine
void IRAM_ATTR onTimerInterrupt() {
  portENTER_CRITICAL_ISR(&myMux);
  update = true;
  portEXIT_CRITICAL_ISR(&myMux);
}

// button interrupt routine
void IRAM_ATTR onButtonInterrupt() {
  portENTER_CRITICAL_ISR(&myMux);
  numberOfButtonInterrupts++;
  lastState = digitalRead(BUTTON_1);
  debounceTimeout = xTaskGetTickCount(); 
  portEXIT_CRITICAL_ISR(&myMux);
}



void setup() {
  Serial.begin(115200);
  WiFi.begin(SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("Connected to Wi-Fi network");
  Serial.println(WiFi.localIP().toString());

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);

  configTime(0, 3600, SNTP_TIME_SERVER);
  printLocalTime();

  tft.init();
  tft.setRotation(3);

  tft.fillScreen(TFT_BLACK);

  analogMeter(0); // Draw analogue meter

  // Set up hardware timer
  My_timer = timerBegin(0, 80, true);
  timerAttachInterrupt(My_timer, &onTimerInterrupt, true);
  timerAlarmWrite(My_timer, 1000000, true);           // every second
  timerAlarmEnable(My_timer);

  //button_init();
  pinMode(BUTTON_1, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_1), onButtonInterrupt, FALLING);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  // 1 second timer for clock on screen
  if (update) {
    // update flag
    portENTER_CRITICAL(&myMux);
    update = false;
    portEXIT_CRITICAL(&myMux);

    printLocalTime();
  }

  if (newScreen != currentScreen) {
    portENTER_CRITICAL_ISR(&myMux); // can't change it unless, atomic - Critical section
    currentScreen = newScreen;   
    portEXIT_CRITICAL_ISR(&myMux);

    if (currentScreen == CURRENTPV) {
      analogMeter(pvnow);
    } else if (currentScreen == DAILYPV) {
      analogMeter2(dailypv);
    } else {
      infoPageReset();
      String message = "IP:" + WiFi.localIP().toString();
      tft.setTextColor(TFT_GREY, TFT_BLACK);
      tft.drawString(message, 10, 30, 4); // // Comment out to avoid font 4
    }
      
    printLocalTime();
  }

  // Debounce routine for button
  if (numberOfButtonInterrupts > 0) {
    portENTER_CRITICAL(&myMux);
    save = numberOfButtonInterrupts;
    saveDebounceTimeout = debounceTimeout;
    saveLastState = lastState;
    portEXIT_CRITICAL(&myMux);

    bool currentState = digitalRead(BUTTON_1);

    // Critical if statement to check for debounce
    if ((save != 0) && (currentState == saveLastState) && (millis() - saveDebounceTimeout > DEBOUNCETIME)) {
      if (currentState == LOW) {
        Serial.printf("Button is pressed and debounced, current tick=%d\n", millis());
        if (currentScreen == CURRENTPV) {
          portENTER_CRITICAL_ISR(&myMux); // can't change it unless, atomic - Critical section
          newScreen = DAILYPV;   
          portEXIT_CRITICAL_ISR(&myMux);
        } else if (currentScreen == DAILYPV) {
          portENTER_CRITICAL_ISR(&myMux); // can't change it unless, atomic - Critical section
          newScreen = INFO;   
          portEXIT_CRITICAL_ISR(&myMux);
        } else { // INFO screen
          portENTER_CRITICAL_ISR(&myMux); // can't change it unless, atomic - Critical section
          newScreen = CURRENTPV;   
          portEXIT_CRITICAL_ISR(&myMux);
        } 

      }

      portENTER_CRITICAL_ISR(&myMux); // can't change it unless, atomic - Critical section
      numberOfButtonInterrupts = 0;   // acknowledge keypress and reset interrupt counter
      portEXIT_CRITICAL_ISR(&myMux);

      vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// MQTT callback
void callback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }

    struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.print(&timeinfo, " : %A, %B %d %Y %H:%M:%S");
  
  Serial.println();

  if (String(topic) == "solar/pvnow") {
    portENTER_CRITICAL_ISR(&myMux);
    pvnow = messageTemp.toInt();
    portEXIT_CRITICAL_ISR(&myMux);

    if (currentScreen == CURRENTPV) {
      plotNeedle(pvnow, 0);
    } else if (currentScreen == INFO) {
      String message;
      if (pvnow > 999)
        message = "PV: " + messageTemp + " kW";
      else
        message = "PV: " + messageTemp + " W";

      tft.fillRect(50, 58, 180, 26, TFT_BLACK); // clear out old message as new one could be shorter!
      tft.setTextColor(TFT_GREY, TFT_BLACK);
      tft.drawString(message, 10, 60, 4);
    }
  }   
  
  if (String(topic) == "solar/pvtotal") {
    portENTER_CRITICAL_ISR(&myMux);
    dailypv = messageTemp.toFloat();
    portEXIT_CRITICAL_ISR(&myMux);

    if (currentScreen == DAILYPV) {
      plotNeedle2(dailypv, 0);
    } else if (currentScreen == INFO) {
      String message = "Today: " + messageTemp + " kW";

      tft.fillRect(90, 88, 150, 26, TFT_BLACK);
      tft.setTextColor(TFT_GREY, TFT_BLACK);
      tft.drawString(message, 10, 90, 4); // // Comment out to avoid font 4
    }
  }
}

// #########################################################################
//  Draw the analogue meter on the screen
// #########################################################################
void analogMeter(int value)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREY);  // Text colour

  // Draw ticks every 5 degrees from -50 to +50 degrees (100 deg. FSD swing)
  for (int i = -50; i < 51; i += 5) {
    // Long scale tick length
    int tl = 15;

    // Coodinates of tick to draw
    float sx = cos((i - 90) * 0.0174532925);
    float sy = sin((i - 90) * 0.0174532925);
    uint16_t x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    uint16_t y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    uint16_t x1 = sx * M_SIZE*100 + M_SIZE*120;
    uint16_t y1 = sy * M_SIZE*100 + M_SIZE*150;

    // Coordinates of next tick for zone fill
    float sx2 = cos((i + 5 - 90) * 0.0174532925);
    float sy2 = sin((i + 5 - 90) * 0.0174532925);
    int x2 = sx2 * (M_SIZE*100 + tl) + M_SIZE*120;
    int y2 = sy2 * (M_SIZE*100 + tl) + M_SIZE*150;
    int x3 = sx2 * M_SIZE*100 + M_SIZE*120;
    int y3 = sy2 * M_SIZE*100 + M_SIZE*150;

    // Green zone limits
    if (i >= -50 && i < -20) {
     tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_GREEN);
     tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_GREEN);
    }

    // Orange zone limits
    if (i >= -20 && i < 20) {
      tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_ORANGE);
      tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_ORANGE);
    }

    // Red zone limits
    if (i >= 20 && i < 50) {
      tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_RED);
      tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_RED);
    }

    // Short scale tick length
    if (i % 25 != 0) tl = 8;

    // Recalculate coords incase tick lenght changed
    x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    x1 = sx * M_SIZE*100 + M_SIZE*120;
    y1 = sy * M_SIZE*100 + M_SIZE*150;

    // Draw tick
    tft.drawLine(x0, y0, x1, y1, TFT_GREY);

    // Check if labels should be drawn, with position tweaks
    if (i % 25 == 0) {
      // Calculate label positions
      x0 = sx * (M_SIZE*100 + tl + 10) + M_SIZE*120;
      y0 = sy * (M_SIZE*100 + tl + 10) + M_SIZE*150;
      switch (i / 25) {
        case -2: tft.drawCentreString("0", x0+4, y0-4, 1); break;
        case -1: tft.drawCentreString("1000", x0+2, y0, 1); break;
        case 0: tft.drawCentreString("2000", x0, y0, 1); break;
        case 1: tft.drawCentreString("3000", x0, y0, 1); break;
        case 2: tft.drawCentreString("4000", x0-2, y0-4, 1); break;
      }
    }

    // Now draw the arc of the scale
    sx = cos((i + 5 - 90) * 0.0174532925);
    sy = sin((i + 5 - 90) * 0.0174532925);
    x0 = sx * M_SIZE*100 + M_SIZE*120;
    y0 = sy * M_SIZE*100 + M_SIZE*150;
  
    // Draw scale arc, don't draw the last part
    if (i < 50) tft.drawLine(x0, y0, x1, y1, TFT_GREY);
  }

  tft.drawString("PV Now", M_SIZE*(3 + 230 - 60), M_SIZE*(119 - 20), 2); // Units at bottom right
  tft.drawCentreString("kW", M_SIZE*120, M_SIZE*75, 4); 

  plotNeedle(value, 0); // Put meter needle at 0
}

// #########################################################################
// Update needle position
// This function is blocking while needle moves, time depends on ms_delay
// 10ms minimises needle flicker if text is drawn within needle sweep area
// Smaller values OK if text not in sweep area, zero for instant movement but
// does not look realistic... (note: 100 increments for full scale deflection)
// #########################################################################
void plotNeedle(int newvalue, byte ms_delay)
{
//  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextColor(TFT_GREY, TFT_BLACK);
  char buf[8]; dtostrf(newvalue, 4, 0, buf);
  tft.drawRightString(buf, 40, M_SIZE*(119 - 20), 2);

  int value = newvalue / 40;
  if (value < -10) value = -10; // Limit value to emulate needle end stops
  if (value > 110) value = 110;

  // Move the needle until new value reached
  while (!(value == old_analog)) {
    if (old_analog < value) old_analog++;
    else old_analog--;

    if (ms_delay == 0) old_analog = value; // Update immediately if delay is 0

    float sdeg = map(old_analog, -10, 110, -150, -30); // Map value to angle
    // Calculate tip of needle coords
    float sx = cos(sdeg * 0.0174532925);
    float sy = sin(sdeg * 0.0174532925);

    // Calculate x delta of needle start (does not start at pivot point)
    float tx = tan((sdeg + 90) * 0.0174532925);

    // Erase old needle image
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_BLACK);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_BLACK);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_BLACK);

    // Re-plot text under needle
    tft.setTextColor(TFT_GREY, TFT_BLACK);
    tft.drawCentreString("kW", M_SIZE*120, M_SIZE*75, 4); // // Comment out to avoid font 4

    // Store new needle end coords for next erase
    ltx = tx;
    osx = M_SIZE*(sx * 98 + 120);
    osy = M_SIZE*(sy * 98 + 150);

    Serial.print("ltx:" );
    Serial.print(ltx);
    Serial.print("  osx:");
    Serial.print(osx);
    Serial.print("  osy:");
    Serial.print(osy);
    Serial.println("");

    Serial.print("drawline:" );
    Serial.print(M_SIZE*(120 + 24 * ltx) - 1);
    Serial.print(", ");
    Serial.print( M_SIZE*(150 - 24), osx - 1);
    Serial.print(", ");
    Serial.print(osx-1);
    Serial.print(", ");
    Serial.print(osy);
    Serial.println("");

    // Draw the needle in the new postion, magenta makes needle a bit bolder
    // draws 3 lines to thicken needle
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_RED);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_MAGENTA);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_RED);

    // Slow needle down slightly as it approaches new postion
    if (abs(old_analog - value) < 10) ms_delay += ms_delay / 5;

    // Wait before next update
    delay(ms_delay);
  }
}

// #########################################################################
//  Draw the analogue meter on the screen
// #########################################################################
void analogMeter2(float value)
{
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_GREY);  // Text colour

  // Draw ticks every 5 degrees from -50 to +50 degrees (100 deg. FSD swing)
  for (int i = -50; i < 51; i += 5) {
    // Long scale tick length
    int tl = 15;

    // Coodinates of tick to draw
    float sx = cos((i - 90) * 0.0174532925);
    float sy = sin((i - 90) * 0.0174532925);
    uint16_t x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    uint16_t y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    uint16_t x1 = sx * M_SIZE*100 + M_SIZE*120;
    uint16_t y1 = sy * M_SIZE*100 + M_SIZE*150;

    // Coordinates of next tick for zone fill
    float sx2 = cos((i + 5 - 90) * 0.0174532925);
    float sy2 = sin((i + 5 - 90) * 0.0174532925);
    int x2 = sx2 * (M_SIZE*100 + tl) + M_SIZE*120;
    int y2 = sy2 * (M_SIZE*100 + tl) + M_SIZE*150;
    int x3 = sx2 * M_SIZE*100 + M_SIZE*120;
    int y3 = sy2 * M_SIZE*100 + M_SIZE*150;

    // Green zone limits
    if (i >= -50 && i < -20) {
     tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_GREEN);
     tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_GREEN);
    }

    // Orange zone limits
    if (i >= -20 && i < 20) {
      tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_ORANGE);
      tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_ORANGE);
    }

    // Red zone limits
    if (i >= 20 && i < 50) {
      tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_RED);
      tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_RED);
    }

    // Short scale tick length
    if (i % 25 != 0) tl = 8;

    // Recalculate coords incase tick lenght changed
    x0 = sx * (M_SIZE*100 + tl) + M_SIZE*120;
    y0 = sy * (M_SIZE*100 + tl) + M_SIZE*150;
    x1 = sx * M_SIZE*100 + M_SIZE*120;
    y1 = sy * M_SIZE*100 + M_SIZE*150;

    // Draw tick
    tft.drawLine(x0, y0, x1, y1, TFT_GREY);

    // Check if labels should be drawn, with position tweaks
    if (i % 25 == 0) {
      // Calculate label positions
      x0 = sx * (M_SIZE*100 + tl + 10) + M_SIZE*120;
      y0 = sy * (M_SIZE*100 + tl + 10) + M_SIZE*150;
      switch (i / 25) {
        case -2: tft.drawCentreString("0", x0+4, y0-4, 1); break;
        case -1: tft.drawCentreString("10", x0+2, y0, 1); break;
        case 0: tft.drawCentreString("20", x0, y0, 1); break;
        case 1: tft.drawCentreString("30", x0, y0, 1); break;
        case 2: tft.drawCentreString("40", x0-2, y0-4, 1); break;
      }
    }

    // Now draw the arc of the scale
    sx = cos((i + 5 - 90) * 0.0174532925);
    sy = sin((i + 5 - 90) * 0.0174532925);
    x0 = sx * M_SIZE*100 + M_SIZE*120;
    y0 = sy * M_SIZE*100 + M_SIZE*150;

    // Draw scale arc, don't draw the last part
    if (i < 50) tft.drawLine(x0, y0, x1, y1, TFT_GREY);
  }

  tft.drawString("PV Today", M_SIZE*(3 + 230 - 60), M_SIZE*(119 - 20), 2); // Units at bottom right
  tft.drawCentreString("kW", M_SIZE*120, M_SIZE*75, 4);

  plotNeedle2(value, 0); // Put meter needle at 0
}

// #########################################################################
// Update needle position
// This function is blocking while needle moves, time depends on ms_delay
// 10ms minimises needle flicker if text is drawn within needle sweep area
// Smaller values OK if text not in sweep area, zero for instant movement but
// does not look realistic... (note: 100 increments for full scale deflection)
// #########################################################################
void plotNeedle2(float newvalue, byte ms_delay)
{
//  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextColor(TFT_GREY, TFT_BLACK);
  char buf[8]; dtostrf(newvalue, 4, 2, buf);
  tft.drawRightString(buf, 40, M_SIZE*(119 - 20), 2);

  float value = newvalue * 2.5;
  if (value < -10) value = -10; // Limit value to emulate needle end stops
  if (value > 110) value = 110;

  // Move the needle until new value reached
  while (!(value == old_analog2)) {
    if (old_analog2 < value) old_analog2+=0.1;
    else old_analog2-=0.1;

    if (ms_delay == 0) old_analog2 = value; // Update immediately if delay is 0

    float sdeg = map(old_analog2, -10, 110, -150, -30); // Map value to angle
    // Calculate tip of needle coords
    float sx = cos(sdeg * 0.0174532925);
    float sy = sin(sdeg * 0.0174532925);

    // Calculate x delta of needle start (does not start at pivot point)
    float tx = tan((sdeg + 90) * 0.0174532925);

    // Erase old needle image
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_BLACK);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_BLACK);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_BLACK);

    // Re-plot text under needle
    tft.setTextColor(TFT_GREY, TFT_BLACK);
    tft.drawCentreString("kW", M_SIZE*120, M_SIZE*75, 4); // // Comment out to avoid font 4

    // Store new needle end coords for next erase
    ltx = tx;
    osx = M_SIZE*(sx * 98 + 120);
    osy = M_SIZE*(sy * 98 + 150);

    // Draw the needle in the new postion, magenta makes needle a bit bolder
    // draws 3 lines to thicken needle
    tft.drawLine(M_SIZE*(120 + 24 * ltx) - 1, M_SIZE*(150 - 24), osx - 1, osy, TFT_RED);
    tft.drawLine(M_SIZE*(120 + 24 * ltx), M_SIZE*(150 - 24), osx, osy, TFT_MAGENTA);
    tft.drawLine(M_SIZE*(120 + 24 * ltx) + 1, M_SIZE*(150 - 24), osx + 1, osy, TFT_RED);

    Serial.print("ltx:" );
    Serial.print(ltx);
    Serial.print("  osx:");
    Serial.print(osx);
    Serial.print("  osy:");
    Serial.print(osy);
    Serial.println("");

    Serial.print("drawline:" );
    Serial.print(M_SIZE*(120 + 24 * ltx) - 1);
    Serial.print(", ");
    Serial.print( M_SIZE*(150 - 24), osx - 1);
    Serial.print(", ");
    Serial.print(osx-1);
    Serial.print(", ");
    Serial.print(osy);
    Serial.println("");


    // Slow needle down slightly as it approaches new postion
    if (abs(old_analog2 - value) < 10) ms_delay += ms_delay / 5;

    // Wait before next update
    delay(ms_delay);
  }
}

// #########################################################################
//  Draw the information screen
// #########################################################################
void infoPageReset(){
  // Meter outline
  tft.fillScreen(TFT_BLACK);
}

void informationPage() {

}