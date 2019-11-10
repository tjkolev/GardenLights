#include <Arduino.h>
#include <DS3232RTC.h>
#include <EEPROM.h>
#include <toneAC.h>

#include "main.h"

const char COMPILE_DATETIME[] = __DATE__ " " __TIME__;

#define LED_PIN LED_BUILTIN
#define LIGHTS_PIN A2
#define CHECK_LIGHTS_SECONDS 60
#define TEMP_WARN 50
#define TEMP_CRITICAL 70
#define TEMP_SHUTDOWN 80

#define MAX_INPUT_LEN 31
char inputBuffer[MAX_INPUT_LEN + 1];
#define MSG_BUFF_LEN 128
char msg[MSG_BUFF_LEN] = "";

// Fixed off time is minutes after midnight. Byte size in EEPROM
#define FIXED_OFF_TIME_EEADDR 0
#define FIXED_OFF_TIME_DISABLED 255 // magic number
byte fixedOffTimeOffset = FIXED_OFF_TIME_DISABLED;

// Extra minutes added to sunrise and subtracted from sunset. Byte size in EEPROM
#define XTRA_MINUTES_EEADDR 1
byte xtraMinutes = 0;

const char BAD_INPUT[] = "Bad input.";

void printHelp();
void printTime(time_t &t);
void printInfo();
void printFixedOffTime();
bool isValidDate(int cmonth, int cdate);
bool isValidTime(int chour, int cmin, int csec);

byte storeEeprom(int idxAddr, byte value);
byte loadEeprom(int idxAddr);
void setFixedOffTimeOffset();
void setXtraMinutes();

void inputOverflowNotice();
void timeNotSetNotice();
void setTime();
bool readInput();
int getTemp();
void processInput();
void checkLights();
bool checkTemp();
void lightsOn();
void lightsOff();

void setup() {
  // put your setup code here, to run once:

  toneAC(523, 10, 500);
  toneAC(262, 10, 500);
  toneAC(523, 10, 500);

  pinMode(LED_PIN, OUTPUT);
  pinMode(LIGHTS_PIN, OUTPUT);
  lightsOff();

  Serial.begin(9600);

  if(RTC.oscStopped(false)) {
    Serial.println("RTC lost power. Set time.");
  }
  setSyncProvider(RTC.get);
  
  fixedOffTimeOffset = loadEeprom(FIXED_OFF_TIME_EEADDR);
  xtraMinutes = loadEeprom(XTRA_MINUTES_EEADDR);
  
  printHelp();
}

unsigned long lastCheckMillis = 0;
unsigned long nowMillis = 0;
void loop() {

  // read from serial while waiting for next lights check
  if(readInput()) {
    processInput();
    lastCheckMillis = 0; // Make it check the lights 
  }
 
  nowMillis = millis();
  if((lastCheckMillis == 0) || (nowMillis - lastCheckMillis >= CHECK_LIGHTS_SECONDS * 1000ul)) {
    if(checkTemp()) {
      checkLights();
    }
    lastCheckMillis = nowMillis;
  }
}

void lightsOn() {
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(LIGHTS_PIN, HIGH);
  Serial.println("Lights on.");
}

void lightsOff() {
  digitalWrite(LIGHTS_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  Serial.println("Lights off.");
}

int time2Offset(int HHmm) {
  return (HHmm / 100) * 60 + (HHmm % 100);
}

int offset2Time(int offsetMinutes) {
  return (offsetMinutes / 60 ) * 100 + (offsetMinutes % 60);
}

TimeInfo getTimeInfo(int cmonth, int cdate) {
  TimeInfo ti;

  ti.LightsOff =
  ti.SunRise = pgm_read_word(&SunRiseSet[cmonth-1][cdate-1][0]); // sunrise
  ti.LightsOn =
  ti.SunSet = pgm_read_word(&SunRiseSet[cmonth-1][cdate-1][1]); // sunset

  // adjust time with extra minutes
  if(xtraMinutes > 0) {
    ti.LightsOff =  offset2Time(time2Offset(ti.LightsOff) + xtraMinutes);
    ti.LightsOn = offset2Time(time2Offset(ti.LightsOn) - xtraMinutes);
  }

  // Lights off time could be fixed time.
  if(fixedOffTimeOffset != FIXED_OFF_TIME_DISABLED) {
    ti.LightsOff = offset2Time(fixedOffTimeOffset);
  }

  return ti;
}

void checkLights(){
  
  if(timeSet != timeStatus()) {
    Serial.println("Can't check lights: time has not been set.");
    timeNotSetNotice();
    return;
  }

  time_t t = now();
  int cmonth, cdate;
  cmonth = month(t);
  cdate = day(t);
  TimeInfo ti = getTimeInfo(cmonth, cdate);
  int ctime = hour(t) * 100 + minute(t);
  if(ti.LightsOff <= ctime && ctime <= ti.LightsOn) {
    lightsOff();
  }
  else {
    lightsOn();
  }
}

bool coolingOff = false;
bool checkTemp() {
  int temp = getTemp();

  if(temp >= TEMP_SHUTDOWN) {
    lightsOff();
    coolingOff = true;
    for(int n=0;n<6;n++) toneAC(1200, 10, 300);
    return false;
  }
  
  if(temp >= TEMP_CRITICAL) {
    for(int n=0;n<4;n++) toneAC(1200, 10, 600);
    return !coolingOff;
  }

  if(temp >= TEMP_WARN) {
    for(int n=0;n<2;n++) toneAC(1200, 10, 1000);
    return !coolingOff;
  }

  coolingOff = false;
  return true;
}

void printHelp() {
  Serial.print("GardenLights, "); Serial.println(COMPILE_DATETIME);
  Serial.println("h: Help.");
  Serial.println("i[yyyyMMdd]: Date info.");
  Serial.println("tyyyyMMddHHmmss: Set time and date.");
  Serial.println("f[mmm]: Fixed off time.");
  Serial.println("x[mmm]: Extra minutes.");
}

void processInput() {
  char cmd = inputBuffer[0];
  switch(cmd) {
    case 'h':
      printHelp();
      break;
    
    case 'i':
      printInfo();
      break;
    
    case 't':
      setTime();
      break;    
    
    case 'f':
      setFixedOffTimeOffset();
      break;
        
    case 'x':
      setXtraMinutes();
      break;

    default:
      Serial.println(BAD_INPUT);
      break;
  }
}

bool readInput() {
  static int ndx = 0;

  while (Serial.available() > 0) {
    char receivedChar = Serial.read();
    
    if (receivedChar == '\r') {
      // ignore
    }
    else if (receivedChar == 0x08) {
      // backspace clears input
      ndx = 0;
      Serial.println("\nInput canceled.");
    }
    else if (receivedChar == '\n') {
      // end of input
      inputBuffer[ndx] = '\0'; // end of string
      ndx = 0;
      Serial.print(receivedChar);
      return true;
    }
    else {
      if (ndx < MAX_INPUT_LEN) {
        inputBuffer[ndx++] = receivedChar;
        Serial.print(receivedChar);
      }
      else {
        inputOverflowNotice();
      }
    }
  }

  return false;
}

byte storeEeprom(int idxAddr, byte value) {
  EEPROM.update(idxAddr, value);
  return value;
}

byte loadEeprom(int idxAddr) {
  return EEPROM.read(idxAddr);
}

int getTemp() {
  return RTC.temperature() / 4;
}

void inputOverflowNotice() {
  Serial.println("\nInput overflow.");
  toneAC(400, 10, 200);
}

void timeNotSetNotice() {
  toneAC(600, 10, 600);
  toneAC(400, 10, 400);
}

bool isValidDate(int cmonth, int cdate) {
  return !(cmonth < 1 || cmonth > 12 
    || cdate < 1 || cdate > 31 
    || (cdate > 30 && (cmonth == 4 || cmonth == 6 || cmonth == 9 || cmonth == 11)) 
    || (cdate > 29 && cmonth == 2));
}

bool isValidTime(int chour, int cmin, int csec) {
  return chour >= 0 && chour <= 23
        && cmin >= 0 && cmin <= 59
        && csec >=0 && csec <= 59;
}

void printTime(time_t &t) {
  snprintf(msg, MSG_BUFF_LEN, "It is %04d-%02d-%02d %02d:%02d:%02d", year(t), month(t), day(t), hour(t), minute(t), second(t));
  Serial.println(msg);
}

void printInfo() {
  int inputLen = strlen(inputBuffer);
  if(1 != inputLen && 9 != inputLen) {
    Serial.println(BAD_INPUT);
    return;
  }

  if(timeSet != timeStatus()) {
    Serial.println("Time has not been set.");
    return;
  }

  time_t t = now();
  int cyear, cmonth, cdate;
  if(1 == inputLen) {
    cyear = year(t);
    cmonth = month(t);
    cdate = day(t);
  }
  else if(9 == inputLen) {
    sscanf(inputBuffer + 1, "%4d%02d%02d", &cyear, &cmonth, &cdate);
    if(!isValidDate(cmonth, cdate)) {
      Serial.println(BAD_INPUT);
      return;
    }
  }

  TimeInfo ti = getTimeInfo(cmonth, cdate);

  printTime(t);
  snprintf(msg, MSG_BUFF_LEN, "For     %04d-%02d-%02d", cyear, cmonth, cdate);
  Serial.println(msg);
  snprintf(msg, MSG_BUFF_LEN, "Sunrise %02d:%02d", ti.SunRise / 100, ti.SunRise % 100);
  Serial.println(msg);
  snprintf(msg, MSG_BUFF_LEN, "Sunset  %02d:%02d", ti.SunSet / 100, ti.SunSet % 100);
  Serial.println(msg);
  printFixedOffTime();
  snprintf(msg, MSG_BUFF_LEN, "Extra minutes %d", xtraMinutes);
  Serial.println(msg);
  snprintf(msg, MSG_BUFF_LEN, "Lights Off %02d:%02d", ti.LightsOff / 100, ti.LightsOff % 100);
  Serial.println(msg);
  snprintf(msg, MSG_BUFF_LEN, "Lights On  %02d:%02d", ti.LightsOn / 100, ti.LightsOn % 100);
  Serial.println(msg);
  snprintf(msg, MSG_BUFF_LEN, "Temp %d C", getTemp());
  Serial.println(msg);
}

void setTime() {
  int inputLen = strlen(inputBuffer);
  if(15 != inputLen) {
    Serial.println(BAD_INPUT);
    return;
  }

  int cyear, cmonth, cdate, chour, cmin, csec;
  sscanf(inputBuffer + 1, "%4d%02d%02d%02d%02d%02d", &cyear, &cmonth, &cdate, &chour, &cmin, &csec);
  if(!isValidDate(cmonth, cdate) || !isValidTime(chour, cmin, csec)) {
    Serial.println(BAD_INPUT);
    return;
  }

  tmElements_t tm;
  tm.Year = CalendarYrToTm(cyear);
  tm.Month = cmonth;
  tm.Day = cdate;
  tm.Hour = chour;
  tm.Minute = cmin;
  tm.Second = csec;
  time_t newTime = makeTime(tm);
  setTime(newTime);
  byte ok = RTC.set(newTime);
  if(0 != ok) {
    Serial.print("Failed to set RTC: ");Serial.println(ok);
  }

  time_t tnow = now();
  if(timeSet != timeStatus()) {
    Serial.println("Failed to set time.");
    return;
  }

  printTime(tnow);
}

void printFixedOffTime() {
  Serial.print("Fixed off time set to ");
  if(fixedOffTimeOffset == FIXED_OFF_TIME_DISABLED) {
    Serial.println("'disabled'.");
    return;
  }

  int hour = fixedOffTimeOffset / 60;
  int minute = fixedOffTimeOffset % 60;
  snprintf(msg, MSG_BUFF_LEN, "%02d:%02d", hour, minute);
  Serial.println(msg);
}

void setFixedOffTimeOffset() {
  int inputLen = strlen(inputBuffer);
  if(inputLen < 1 || inputLen > 4) {
    Serial.println(BAD_INPUT);
    return;
  }

  int offsetVal;
  sscanf(inputBuffer + 1, "%d", &offsetVal);
  if(offsetVal < 1 || offsetVal > 255) {
    Serial.println(BAD_INPUT);
    return;
  }

  fixedOffTimeOffset = storeEeprom(FIXED_OFF_TIME_EEADDR, offsetVal);
}

void setXtraMinutes() {
  int inputLen = strlen(inputBuffer);
  if(inputLen < 1 || inputLen > 4) {
    Serial.println(BAD_INPUT);
    return;
  }

  int xmin;
  sscanf(inputBuffer + 1, "%d", &xmin);
  if(xmin < 0 || xmin > 255) {
    Serial.println(BAD_INPUT);
    return;
  }

  xtraMinutes = storeEeprom(XTRA_MINUTES_EEADDR, xmin);
}
