#include <SD.h>
#include "Wire.h"
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <EEPROM.h>
#include "Adafruit_MAX31855.h"
#include "Adafruit_MAX31856.h"
#include <Bounce2.h>

#define B_STARTSTOP PB12
#define B_MODE      PB13
#define B_SEL       PB14
#define B_INC       PB15
#define B_DEC       PA8
#define FAN_PWM     PA1
#define SD_CS       PB0
#define ENVTEMP_CS  PA3
#define BEANTEMP_CS PA4
#define HEATER      PB11

Adafruit_MAX31855 thermocouple(BEANTEMP_CS);
Adafruit_MAX31856 envTempSens = Adafruit_MAX31856(ENVTEMP_CS);

LiquidCrystal_I2C lcd(0x27,20,4);  

Bounce startStopButton = Bounce();
Bounce modeButton = Bounce();
Bounce selButton = Bounce();
Bounce incButton = Bounce();
Bounce decButton = Bounce();

enum Modes {
  AUTO,
  MANUAL,
  SETTINGS,
  PIDTUNE_E,
  PIDTUNE_B
} mode;

enum EditVal {
  TEMP,
  FAN,
  CVAR,
  SEG,
  TIME,
  KP,
  KI,
  KD
} editVal;

enum ControlVar {
  ENV,
  BEAN
} controlVar;

// EEPROM (Flash memory) Addresses
#define ADDR_KP_E 0  
#define ADDR_KI_E 2
#define ADDR_KD_E 4
#define ADDR_KP_B 6
#define ADDR_KI_B 8
#define ADDR_KD_B 10
#define ADDR_SEG_TIME 12 // Address 12 - 29 (9 segments x 2 bytes)
#define ADDR_SEG_TEMP 30 // Addresses 30 - 47


// Control gains are represented as integers ranging from 0 - 999, with LSBs below
#define KP_LSB 0.02  // LSB = 0.02% per deg, 20% per deg full range
#define KI_LSB 0.005  // LSB = 0.005% per deg-sec, 5% per deg-sec full range
#define KD_LSB 0.05   // LSB = 0.05% per deg/sec, 50% per deg/sec full range

class PidController {
  public:
  uint16_t Kp, Ki, Kd;
  void setPidGains(uint16_t Kp_in, uint16_t Ki_in, uint16_t Kd_in);
  double calcControl(double measTemp);
  void reset();
  
  private:
  double intErr = 0, errPrev = 0;  
};

PidController envController, beanController;

int numSeg = 9;
int currentSeg = 1;
int currentSegTime, currentSetTemp;

uint16_t segTime[9], segTemp[9];

double envTemp =  0; // Current temperature measurements
double beanTemp = 0;

int envTempErrCtr = 0, beanTempErrCtr = 0; // Error counters
#define ERR_CTR_TIMEOUT 6

double heat = 0;
double fan = 0;

#define MIN_FAN_SPEED 20  // Minimum fan speed when heater is on

uint8_t heaterState;

unsigned long int secTimer = 0;
uint16_t maxduty;

char filename[] = "LOG000.CSV";
File logfile;

void setup() {
  Serial.begin(115200);
  pinMode(B_STARTSTOP, INPUT_PULLUP);
  pinMode(B_MODE, INPUT_PULLUP);
  pinMode(B_SEL, INPUT_PULLUP);
  pinMode(B_INC, INPUT_PULLUP);
  pinMode(B_DEC, INPUT_PULLUP);

  pinMode(HEATER, OUTPUT);
  digitalWrite(HEATER, LOW);
  heaterState = LOW;
  
  startStopButton.attach(B_STARTSTOP);
  startStopButton.interval(25);

  modeButton.attach(B_MODE);
  modeButton.interval(25);

  selButton.attach(B_SEL);
  selButton.interval(25);

  incButton.attach(B_INC);
  incButton.interval(25);

  decButton.attach(B_DEC);
  decButton.interval(25);

  HardwareTimer pwmtimer(2);  // PA1 on timer 2, channel 2
  pinMode(FAN_PWM, PWM);
  pwmtimer.setPrescaleFactor(1);
  maxduty = pwmtimer.setPeriod(100);  // 100 us = 10 kHz
  setFanSpeed();

  lcd.init();
  lcd.setBacklight(HIGH);
  lcd.setCursor(0,1);
  lcd.print("Looking for SD Card");
  
  while (SD.begin(SD_CS) != true)
  {
    delay(10);
  }
  lcd.clear();
  

  lcd.setCursor(3, 0);
  lcd.print("COFFEE ROASTER");
  lcd.setCursor(9, 2);
  lcd.print("by");
  lcd.setCursor(3,3);
  lcd.print("Larry McGovern");

  delay(2000);

  EEPROM.init();
  // Load control gains
  uint16_t Kp, Ki, Kd;
  EEPROM.read(ADDR_KP_E, &Kp);  
  EEPROM.read(ADDR_KI_E, &Ki);
  EEPROM.read(ADDR_KD_E, &Kd);
  if (Kp < 0 || Kp > 999) Kp = 0;
  if (Ki < 0 || Ki > 999) Ki = 0;
  if (Kd < 0 || Kd > 999) Kd = 0;
  envController.setPidGains(Kp, Ki, Kd);
  
  EEPROM.read(ADDR_KP_B, &Kp);
  EEPROM.read(ADDR_KI_B, &Ki);
  EEPROM.read(ADDR_KD_B, &Kd);
  if (Kp < 0 || Kp > 999) Kp = 0;
  if (Ki < 0 || Ki > 999) Ki = 0;
  if (Kd < 0 || Kd > 999) Kd = 0;
  beanController.setPidGains(Kp, Ki, Kd);

  // Load segment times and temperatures
  for (int i = 0; i < 9; i++) {
    EEPROM.read(ADDR_SEG_TIME, &segTime[i]);
    EEPROM.read(ADDR_SEG_TEMP, &segTemp[i]);
    if (segTime[i] < 0 || segTime[i] > 6000) segTime[i] = 30;
    if (segTemp[i] < 0 || segTemp[i] > 500)  segTemp[i] = 100;
  }
  currentSeg = 1;
  currentSegTime = segTime[0];
  currentSetTemp = segTemp[0];
  
  envTempSens.begin();
  //envTempSens.setThermocoupleType(MAX31856_TCTYPE_K); // Already default in .begin();

  mode = AUTO;
  editVal = TEMP;
  controlVar = ENV;
  
  lcd.clear();
  secTimer = millis();
  updateDisplay();
}

unsigned long int elapsedTimeStart = 0;
float elapsedTime = 0.0;

bool roast = false;
bool savedata = false;

/*
 * Main Loop
 */
void loop() {
  static bool incState, decState;
  static unsigned long incButtonPressTimeStamp1, decButtonPressTimeStamp1;
  static unsigned long incButtonPressTimeStamp2, decButtonPressTimeStamp2;
  int rate;
  //static double intErr = 0, errPrev = 0;

  /*
   * Button Logic
   */
  startStopButton.update();
  if (startStopButton.fell()) {
    roast = !roast;
    if (roast) { // If starting new roast
        elapsedTimeStart = millis();
        for (uint16_t i = 0; i < 999; i++) {
          filename[3] = i / 100 + '0';
          filename[4] = i / 10 % 10 + '0';
          filename[5] = i % 10 + '0';
          if (! SD.exists(filename)) {
            // only open a new file if it doesn't exist
            logfile = SD.open(filename, FILE_WRITE);
            break;  // leave the loop!
          }
      }
      logfile.println("Time, Control Temp, Env Temp, Bean Temp, Heat, Fan");
      logfile.flush();
    }
    else {
      envController.reset();
      beanController.reset();
      logfile.close();
    }
  }

  modeButton.update();
  if (modeButton.fell()) {  // Switch Modes
    if (!roast) { // Only switch modes if not roasting
      if (mode == AUTO){
        mode = MANUAL;
        editVal = TEMP;
      }
      else if (mode == MANUAL){
        mode = SETTINGS;
        editVal = SEG;
      }
      else if (mode == SETTINGS){
        mode = PIDTUNE_E;
        editVal = KP;
      }
      else if (mode == PIDTUNE_E){
        mode = PIDTUNE_B;
        if (savedata) {
          EEPROM.write(ADDR_KP_E, envController.Kp);
          EEPROM.write(ADDR_KI_E, envController.Ki);
          EEPROM.write(ADDR_KD_E, envController.Kd);
          // Add a screen to display that data was saved
        }
      }
      else if (mode == PIDTUNE_B){
        mode = AUTO;
        if (savedata) {
          EEPROM.write(ADDR_KP_B, beanController.Kp);
          EEPROM.write(ADDR_KI_B, beanController.Ki);
          EEPROM.write(ADDR_KD_B, beanController.Kd);
          // Add a screen to display that data was saved
        }
      }
      savedata = false;
    }
    lcd.clear();
    updateDisplay();
  }

  selButton.update();
  if (selButton.fell()) {  // Make new selection (MANUAL and SETTINGS modes)
    if (mode == MANUAL) {
      if (editVal == TEMP)  editVal = FAN;
      else if (editVal == FAN) editVal = CVAR;
      else editVal = TEMP;
    }
    else if (mode == SETTINGS) {
      if (editVal == SEG) editVal = TIME;
      else if (editVal == TIME) editVal = TEMP;
      else if (editVal == TEMP) editVal = FAN;
      else if (editVal == FAN) editVal = CVAR;
      else editVal = SEG;
    }
    else if ((mode == PIDTUNE_E) || (mode == PIDTUNE_B)) {
      if (editVal == KP) editVal = KI;
      else if (editVal == KI) editVal = KD;
      else if (editVal == KD) editVal = TEMP;  
      else if (editVal == TEMP) editVal = FAN;  
      else if (editVal == FAN) editVal = KP;      
    }
    updateDisplay();
  }

  incButton.update();
  if (incButton.fell()) { // Button was pushed
      if (editVal == CVAR) {
        controlVar = (controlVar == ENV ? BEAN : ENV);
        envController.reset();
        beanController.reset();
        updateDisplay();
      }
      else {
        incState = true;
        incButtonPressTimeStamp1 = millis();
        incButtonPressTimeStamp2 = incButtonPressTimeStamp1;       
      }
    }
  else if (incButton.read() == HIGH) { // Button has been released
      incState = false;
  }

  if (incState) {
    if (millis() - incButtonPressTimeStamp1 >= 4000)  // If button depressed for longer than 4 seconds, then increment faster
      rate = 50;
    else
      rate = 500;
    if (millis() - incButtonPressTimeStamp2 >= rate) {
      lcd.setCursor(0,0);
      incDecSelection(1);
      incButtonPressTimeStamp2 = millis();
    }
  }
  
  decButton.update();
  if (decButton.fell()) { // Button was pushed
      if (editVal == CVAR) {
        controlVar = (controlVar == ENV ? BEAN : ENV);
        envController.reset();
        beanController.reset();
        updateDisplay();
      }
      else {
        decState = true;
        decButtonPressTimeStamp1 = millis();
        decButtonPressTimeStamp2 = decButtonPressTimeStamp1;       
      }
    }
  else if (decButton.read() == HIGH) { // Button has been released
      decState = false;
  }

  if (decState) {
    if (millis() - decButtonPressTimeStamp1 >= 4000)
      rate = 50;
    else
      rate = 500;
    if (millis() - decButtonPressTimeStamp2 >= rate) {
      incDecSelection(-1);
      decButtonPressTimeStamp2 = millis();
    }
  }

  /*
   *  Read sensors and update display.  If roasting, update control and save to SD Card
   */

  static bool firstMeas = false;  // Temperatures measured twice every second and averaged
  static double envTempMeas1 = NAN, beanTempMeas1 = NAN;
  static double envTempAve = 0, beanTempAve = 0;
  
  if ((millis() - secTimer >= 500) && (firstMeas == false)) { 
    readTempSensors();
    
    if (envTempErrCtr == 0)  envTempMeas1 = envTemp;    
    else envTempMeas1 = NAN;  
    
    if (beanTempErrCtr == 0) beanTempMeas1 = beanTemp;    
    else beanTempMeas1 = NAN;  
      
    updateDisplay();
    updateSD();
    
    firstMeas = true;
  }
  
  if (millis() - secTimer >= 1000) {
    secTimer = millis();
    
    if (heat > 0) heaterState = HIGH;
    else  heaterState = LOW;
    digitalWrite(HEATER, heaterState);  // Turn on heater at beginning of 1 second interval
    
    readTempSensors();

    if (!isnan(envTempMeas1) && (envTempErrCtr == 0) ) // Two valid measurements
      envTempAve = 0.5 * (envTempMeas1 + envTemp);    // Calculate average
    else
      envTempAve = envTemp;  // Otherwise, set to last valid measurement

    if (!isnan(beanTempMeas1) && (beanTempErrCtr == 0) ) // Two valid measurements
      beanTempAve = 0.5 * (beanTempMeas1 + beanTemp);    // Calculate average
    else
      envTempAve = envTemp;  // Otherwise, set to last valid measurement

    // Calculate feedback control
    heat = 0;  // Heat off by default
    if (roast) {
      if ((mode == PIDTUNE_E) || ((mode != PIDTUNE_B) && (controlVar == ENV))) { // Env temp control
        if (envTempErrCtr < ERR_CTR_TIMEOUT) {
          heat = envController.calcControl(envTempAve);
        }
      }
      else { // Bean temp control
        if (beanTempErrCtr < ERR_CTR_TIMEOUT) {
          heat = beanController.calcControl(beanTempAve);
        }
      }
    }

    if ((heat > 0) && (fan < MIN_FAN_SPEED)) {
        fan = MIN_FAN_SPEED;
    }
    setFanSpeed();
    
    updateDisplay();
    updateSD();

    firstMeas = false;
  }

  heaterDutyCycleOff();
  
}

void heaterDutyCycleOff() {
  int dc = (int)(10*heat);
  if (heat >= 100.0) return;  // Will leave heater on no matter what
  if ((millis() - secTimer + 100 > dc) && (heaterState == HIGH)) { // Check if we need to turn off heater within the next 100 ms
    while (millis() - secTimer < dc); // Just wait until the right time
    Serial.print("Turn off timing: ");
    Serial.println(millis() - secTimer - (int)(10*heat));
    heaterState = LOW;
    digitalWrite(HEATER, LOW);
  }
}
/*
 * Read temperature sensors
 */
void readTempSensors() {
  double envTempC = envTempSens.readThermocoupleTemperature();
  if (isnan(envTempC)) {  
    envTempErrCtr++;
  }
  else {
    envTemp = 32.0f + 1.8f * envTempC;
    envTempErrCtr = 0;
  }

  double beanTempC = thermocouple.readCelsius();
  if (isnan(beanTempC)) {  
    beanTempErrCtr++;
  }
  else {
    beanTemp = 32.0f + 1.8f * beanTempC;
    beanTempErrCtr = 0;
  }

  if (envTempErrCtr >= ERR_CTR_TIMEOUT) envTemp = NAN;
  if (beanTempErrCtr >= ERR_CTR_TIMEOUT) beanTemp = NAN;
   
  Serial.print(envTemp);  Serial.print(" ");
  Serial.print(beanTemp);  Serial.print(" ");
  Serial.print(envTempErrCtr);  Serial.print(" ");
  Serial.print(beanTempErrCtr);
  
}
 
/*
 * Update SD Card
 */
void updateSD() {
    heaterDutyCycleOff();  
    if (roast) {  //logfile.println("Time, Env Temp, Bean Temp, Heat, Fan");
      elapsedTime = (float)(millis() - elapsedTimeStart)/1000.0f;
      logfile.print(elapsedTime,1);  
      logfile.print(", ");  
      logfile.print(currentSetTemp);       
      logfile.print(", ");
      logfile.print(envTemp,1);
      logfile.print(", ");
      logfile.print(beanTemp,1);
      logfile.print(", ");
      logfile.print(heat);
      logfile.print(", ");
      logfile.println(fan);
      logfile.flush();
    }
}
/*
 * Update Display
 */
void updateDisplay() {
  char timeStr[5];

  heaterDutyCycleOff();
  
   if (mode == SETTINGS) {  // Unique display for settings
      lcd.setCursor(0,0);
      lcd.print("AUTO SETTINGS"); 
      showSelection();
   }
    
   if (mode == AUTO) { // Unique display for AUTO
      lcd.setCursor(0,0);
      lcd.print("AUTO ROAST");      
   }

   if (mode == AUTO || mode == SETTINGS) { // Common for both AUTO and SETTINGS
      lcd.setCursor(1,1);
      lcd.print(currentSeg);
      lcd.print("/");
      lcd.print(numSeg);
      lcd.setCursor(5,1);
      formattedTime(timeStr,currentSegTime); 
      lcd.print(timeStr);    
   }
   
   if (mode == MANUAL) {
      // Unique display for MANUAL
      lcd.setCursor(0,0);
      lcd.print("MANUAL ROAST");  
      showSelection();
   }

   if ((mode == PIDTUNE_E) || (mode == PIDTUNE_B)) {
      // Unique display for PIDTUNE
      char str[3];
      uint16_t Kp, Ki, Kd;
      lcd.setCursor(0,0);
      if (mode == PIDTUNE_E) {
        lcd.print("ENV ");
        Kp = envController.Kp;
        Ki = envController.Ki;
        Kd = envController.Kd;
      }
      else {
        lcd.print("BEAN ");
        Kp = beanController.Kp;
        Ki = beanController.Ki;
        Kd = beanController.Kd;
      }
      lcd.print("PID TUNE"); 
      lcd.setCursor(1,1);
      lcd.print("P");
      sprintf(str,"%03d", Kp);
      lcd.print(str);
      lcd.setCursor(6,1);
      lcd.print("I");
      sprintf(str,"%03d", Ki);
      lcd.print(str);
      lcd.setCursor(11,1);
      lcd.print("D");
      sprintf(str,"%03d", Kd);
      lcd.print(str);
      lcd.setCursor(16,1);
      lcd.print("T");
      sprintf(str,"%03d", currentSetTemp);
      lcd.print(str);
      
      showSelectionPID();
   }

   if (mode != SETTINGS) {  // Common to all except SETTINGS
      lcd.setCursor(15,0);
      formattedTime(timeStr,(int)elapsedTime); 
      lcd.print(timeStr); 
      if ((mode != PIDTUNE_E) && (mode != PIDTUNE_B)) {
        lcd.setCursor(15,1);
        printTemp(currentSetTemp,0);
        lcd.setCursor(11,2);
        if (controlVar == ENV) lcd.print("*");
        else lcd.print(" ");
        lcd.setCursor(11,3);
        if (controlVar == ENV) lcd.print(" ");
        else lcd.print("*");        
      }
      lcd.setCursor(5,2);
      printPercent((int)heat);
      lcd.setCursor(5,3);
      printPercent((int)fan);      
   }

   // Common display for all modes
   if ((mode != PIDTUNE_E) && (mode != PIDTUNE_B)) {
     lcd.setCursor(12,1); 
     lcd.print("ST");
   }
   lcd.setCursor(0,2);
   lcd.print("HEAT");

   lcd.setCursor(0,3);
   lcd.print("FAN");
 
   lcd.setCursor(12,2);
   lcd.print("ET");
   lcd.setCursor(15,2);
   printTemp((int)envTemp,envTempErrCtr); 
    
   lcd.setCursor(12,3);
   lcd.print("BT"); 
   lcd.setCursor(15,3);
   printTemp((int)beanTemp,envTempErrCtr);  
}

void formattedTime(char *t, int seconds)
{
  int minutes = seconds / 60;
  seconds -= minutes * 60;
  
  sprintf(t, "%s%d%s%d", minutes < 10 ? " " : "", minutes, 
          seconds < 10 ? ":0" : ":", seconds);  // Minutes, seconds
}

void printTemp(int temp, int errCtr) {
  if (errCtr < ERR_CTR_TIMEOUT) {
    if (temp < 100)  lcd.print(" ");
    lcd.print(temp);
  }
  else  lcd.print("---");
  lcd.print((char)223); // Deg symbol
  lcd.print("F");
}

void printPercent(int p) {
  if (p < 100) lcd.print(" ");
  if (p < 10)  lcd.print(" ");
  lcd.print(p);
  lcd.print("%");
}

void showSelection() {
  switch (editVal) {
    case SEG:
      lcd.setCursor(0,1);
      lcd.print(">");
      lcd.setCursor(4,1);
      lcd.print(" ");
      lcd.setCursor(14,1); 
      lcd.print(" ");
      lcd.setCursor(10,2);
      lcd.print(" ");
      lcd.setCursor(4,3);
      lcd.print(" ");
      lcd.setCursor(10,3);
      lcd.print(" ");
      break;
    case TIME:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(4,1);
      lcd.print(">");
      lcd.setCursor(14,1); 
      lcd.print(" ");
      lcd.setCursor(10,2);
      lcd.print(" ");
      lcd.setCursor(4,3);
      lcd.print(" ");
      lcd.setCursor(10,3);
      lcd.print(" ");
      break;    
    case TEMP:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(4,1);
      lcd.print(" ");
      lcd.setCursor(14,1); 
      lcd.print(">");
      lcd.setCursor(10,2);
      lcd.print(" ");
      lcd.setCursor(4,3);
      lcd.print(" ");
      lcd.setCursor(10,3);
      lcd.print(" ");
      break; 
    case FAN:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(4,1);
      lcd.print(" ");
      lcd.setCursor(14,1); 
      lcd.print(" ");
      lcd.setCursor(10,2);
      lcd.print(" ");
      lcd.setCursor(4,3);
      lcd.print(">");
      lcd.setCursor(10,3);
      lcd.print(" ");
      break; 
    case CVAR:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(4,1);
      lcd.print(" ");
      lcd.setCursor(14,1); 
      lcd.print(" ");
      lcd.setCursor(10,2);
      lcd.print(">");
      lcd.setCursor(4,3);
      lcd.print(" ");
      lcd.setCursor(10,3);
      lcd.print(">");
  }
}

void showSelectionPID() {
   switch (editVal) {
    case KP:
      lcd.setCursor(0,1);
      lcd.print(">");
      lcd.setCursor(5,1);
      lcd.print(" ");
      lcd.setCursor(10,1);
      lcd.print(" ");   
      lcd.setCursor(15,1); 
      lcd.print(" ");  
      lcd.setCursor(4,3);
      lcd.print(" ");
      break;
    case KI:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(5,1);
      lcd.print(">");
      lcd.setCursor(10,1);
      lcd.print(" ");   
      lcd.setCursor(15,1); 
      lcd.print(" ");  
      lcd.setCursor(4,3);
      lcd.print(" ");  
      break; 
    case KD:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(5,1);
      lcd.print(" ");
      lcd.setCursor(10,1);
      lcd.print(">");   
      lcd.setCursor(15,1); 
      lcd.print(" ");  
      lcd.setCursor(4,3);
      lcd.print(" ");
      break;
    case TEMP:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(5,1);
      lcd.print(" ");
      lcd.setCursor(10,1);
      lcd.print(" ");   
      lcd.setCursor(15,1); 
      lcd.print(">");  
      lcd.setCursor(4,3);
      lcd.print(" ");
      break;
    case FAN:
      lcd.setCursor(0,1);
      lcd.print(" ");
      lcd.setCursor(5,1);
      lcd.print(" ");
      lcd.setCursor(10,1);
      lcd.print(" ");   
      lcd.setCursor(15,1); 
      lcd.print(" ");  
      lcd.setCursor(4,3);
      lcd.print(">");
   }
}

void incDecSelection(int incDec) {
  savedata = true; // Will write to EEPROM after changing modes
  uint16_t Kp, Ki, Kd;
  
  switch (editVal) {
    case SEG:
      currentSeg = currentSeg + (incDec > 1 ? 1 : -1);
      if (currentSeg > 9) currentSeg = 9;
      else if (currentSeg < 1) currentSeg = 1;
      break;
    case TIME:
      // add later
      break;
    case TEMP:
      int tempIncSize;
      if ((mode == PIDTUNE_E) || (mode == PIDTUNE_B))
        tempIncSize = 5; // Use larger step sizes when tuning PID for sharper step response
      else tempIncSize = 1;
      currentSetTemp = currentSetTemp + (incDec > 0 ? tempIncSize : -tempIncSize);
      if (currentSetTemp > 500) currentSetTemp = 500;
      else if (currentSetTemp < 20) currentSetTemp = 20;
      break;
    case FAN:
      fan = fan + (incDec > 0 ? 5 : -5);
      if (fan > 100) fan = 100;
      else if (fan < 0) fan = 0;

      if ((heat > 0) && (fan < MIN_FAN_SPEED)) {
        fan = MIN_FAN_SPEED;
      }
      setFanSpeed();
      break;
    case KP:
      if (mode == PIDTUNE_E) Kp = envController.Kp;
      else Kp = beanController.Kp;
      if (Kp == 0 && incDec < 0) break;
      Kp = Kp + (incDec > 0 ? 1 : -1);
      if (Kp > 999) Kp = 999;
      if (mode == PIDTUNE_E) envController.Kp = Kp;
      else beanController.Kp = Kp;
      break;
    case KI:
      if (mode == PIDTUNE_E) Ki = envController.Ki;
      else Ki = beanController.Ki;
      if (Ki == 0 && incDec < 0) break;
      Ki = Ki + (incDec > 0 ? 1 : -1);
      if (Ki > 999) Ki = 999;
      if (mode == PIDTUNE_E) envController.Ki = Ki;
      else beanController.Ki = Ki;
      break;   
    case KD:
      if (mode == PIDTUNE_E) Kd = envController.Kd;
      else Kd = beanController.Kd;
      if (Kd == 0 && incDec < 0) break;
      Kd = Kd + (incDec > 0 ? 1 : -1);
      if (Kd > 999) Kd = 999;
      if (mode == PIDTUNE_E) envController.Kd = Kd;
      else beanController.Kd = Kd;
      break; 
  }
}

void setFanSpeed() {
  pwmWrite(FAN_PWM, fan * (double) maxduty / 100.0f);
}

// PID Controller
double PidController::calcControl(double measTemp) {
  double err = (double)currentSetTemp - measTemp;
  double dErr = err - errPrev;
  errPrev = err;
  intErr += err;  // Integrated error

  double intErrLim = 100 / (KI_LSB*Ki);
  if (intErr > intErrLim) intErr = intErrLim;  // Don't allow integrated error to contribute more than 100% duty cycle
  if (intErr < 0) intErr = 0;
      
  double ctrlVal = KP_LSB * Kp * err + KI_LSB * Ki * intErr + KD_LSB * Kd * dErr;

  if (ctrlVal > 100) ctrlVal = 100;
  if (ctrlVal < 0) ctrlVal = 0;

  return(ctrlVal);
}      

void PidController::setPidGains(uint16_t Kp_in, uint16_t Ki_in, uint16_t Kd_in) {
  Kp = Kp_in;
  Ki = Ki_in;
  Kd = Kd_in; 
}

void PidController::reset() {
   intErr = 0;
   errPrev = 0;  
}

