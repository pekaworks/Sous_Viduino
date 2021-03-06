//-------------------------------------------------------------------
//
// Sous Vide Controller
// Bill Earl - for Adafruit Industries
//
// Based on the Arduino PID and PID AutoTune Libraries 
// by Brett Beauregard
//
// Adapted for DFRobot Shield 
// by Peka
//------------------------------------------------------------------

// PID Library
#include <PID_v1.h>
#include <PID_AutoTune_v0.h>

// Libraries for the DFRobot LCD Shield
#include <LiquidCrystal.h>
#include <LCDKeypad.h>

// Libraries for the DS18B20 Temperature Sensor
#include <OneWire.h>
#include <DallasTemperature.h>

// So we can save and retrieve settings
#include <EEPROM.h>

// ************************************************
// Pin definitions
// ************************************************

// Output Relay
#define RelayPin 15

// LED pins
#define RedPin 16
#define YellowPin 17
#define GreenPin 18

// One-Wire Temperature Sensor
// (Use GPIO pins for power/ground to simplify the wiring)
#define ONE_WIRE_BUS 11
#define ONE_WIRE_PWR 12
#define ONE_WIRE_GND 13

// ************************************************
// PID Variables and constants
// ************************************************

//Define Variables we'll be connecting to
double Setpoint;
double Input;
double Output;

volatile long onTime = 0;

// pid tuning parameters
double Kp;
double Ki;
double Kd;

// EEPROM addresses for persisted data
const int SpAddress = 0;
const int KpAddress = 8;
const int KiAddress = 16;
const int KdAddress = 24;

//Specify the links and initial tuning parameters
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, DIRECT);

// 10 second Time Proportional Output window
int WindowSize = 10000; 
unsigned long windowStartTime;

// ************************************************
// Auto Tune Variables and constants
// ************************************************
byte ATuneModeRemember=2;

double aTuneStep=500;
double aTuneNoise=1;
unsigned int aTuneLookBack=20;

boolean tuning = false;

PID_ATune aTune(&Input, &Output);


//
//Default increment values
//

float spincrement = 0.1;
float tpincrement = 1.0;
float tiincrement = 0.01;
float tdincrement = 0.01;

// ************************************************
// DiSplay Variables and constants
// ************************************************

LCDKeypad lcd;

int shift = 0;

#define KEYPAD_SHIFT KEYPAD_SELECT

unsigned long lastInput = 0; // last button press

byte degree[8] = // define the degree symbol 
{ 
  B00110, 
  B01001, 
  B01001, 
  B00110, 
  B00000,
  B00000, 
  B00000, 
  B00000 
}; 

const int logInterval = 10000; // log every 10 seconds
long lastLogTime = 0;

// ************************************************
// States for state machine
// ************************************************
enum operatingState { 
  OFF = 0, SETP, RUN, TUNE_P, TUNE_I, TUNE_D, AUTO};
operatingState opState = OFF;


//led blink vars
int ledState = LOW;             
long previousMillis = 0;
long interval = 1000;

// ************************************************
// Sensor Variables and constants
// Data wire is plugged into port 11 on the Arduino

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// arrays to hold device address
DeviceAddress tempSensor;

// ************************************************
// Setup and diSplay initial screen
// ************************************************
void setup()
{
  Serial.begin(9600);

  // Initialize Relay Control:

  pinMode(RelayPin, OUTPUT);    // Output mode to drive relay
  digitalWrite(RelayPin, LOW);  // make sure it is off to start

  // Set up Ground & Power for the sensor from GPIO pins

  pinMode(ONE_WIRE_GND, OUTPUT);
  digitalWrite(ONE_WIRE_GND, LOW);

  pinMode(ONE_WIRE_PWR, OUTPUT);
  digitalWrite(ONE_WIRE_PWR, HIGH);

  // Initialise Status LEDs

  pinMode(RedPin, OUTPUT);       // Output mode to drive LED
  digitalWrite(RedPin, LOW);   // make sure it is off to start
  pinMode(YellowPin, OUTPUT);    // Output mode to drive LED
  digitalWrite(YellowPin, LOW);  // make sure it is off to start
  pinMode(GreenPin, OUTPUT);     // Output mode to drive LED
  digitalWrite(GreenPin, LOW);   // make sure it is off to start

  // Initialize LCD DiSplay 

  lcd.begin(16, 2);
  lcd.createChar(1, degree); // create degree symbol from the binary

  lcd.print(F("  Pekaworks"));
  lcd.setCursor(0, 1);
  lcd.print(F("   Sous Vide"));

  // Start up the DS18B20 One Wire Temperature Sensor

  sensors.begin();
  if (!sensors.getAddress(tempSensor, 0)) 
  {
    lcd.setCursor(0, 1);
    lcd.print(F("Sensor Error"));
  }
  sensors.setResolution(tempSensor, 12);
  sensors.setWaitForConversion(false);

  delay(3000);  // Splash screen

  // Initialize the PID and related variables
  LoadParameters();
  myPID.SetTunings(Kp,Ki,Kd);

  myPID.SetSampleTime(1000);
  myPID.SetOutputLimits(0, WindowSize);

  // Run timer2 interrupt every 15 ms 
  TCCR2A = 0;
  TCCR2B = 1<<CS22 | 1<<CS21 | 1<<CS20;

  //Timer2 Overflow Interrupt Enable
  TIMSK2 |= 1<<TOIE2;
}

// ************************************************
// Timer Interrupt Handler
// ************************************************
SIGNAL(TIMER2_OVF_vect) 
{
  if (opState == OFF)
  {
    digitalWrite(RelayPin, LOW);  // make sure relay is off
  }
  else
  {
    DriveOutput();
  }
}

// ************************************************
// Main Control Loop
//
// All state changes pass through here
// ************************************************
void loop()
{
  // wait for button release before changing state
  while(ReadButtons() != -1) {
  }

  lcd.clear();

  switch (opState)
  {
  case OFF:
    Off();
    break;
  case SETP:
    Tune_Sp();
    break;
  case RUN:
    Run();
    break;
  case TUNE_P:
    TuneP();
    break;
  case TUNE_I:
    TuneI();
    break;
  case TUNE_D:
    TuneD();
    break;
  }
}

// ************************************************
// Initial State - press RIGHT to enter setpoint
// ************************************************
void Off()
{
  myPID.SetMode(MANUAL);
  digitalWrite(RelayPin, LOW);  // make sure it is off
  lcd.print(F("   Pekaworks"));
  lcd.setCursor(0, 1);
  lcd.print(F("   Sous Vide"));
  int buttons = -1;

  while (buttons != KEYPAD_RIGHT)
  {
    buttons = ReadButtons();
  }
  // Prepare to transition to the RUN state
  sensors.requestTemperatures(); // Start an asynchronous temperature reading

  //turn the PID on
  myPID.SetMode(AUTOMATIC);
  windowStartTime = millis();
  opState = RUN; // start control
}

// ************************************************
// Setpoint Entry State
// UP/DOWN to change setpoint
// RIGHT for tuning parameters
// LEFT for OFF
// SHIFT for 10x tuning
// ************************************************
void Tune_Sp()
{

  lcd.print(F("Set Temperature:"));
  int buttons = -1;
  while(true)
  {
    buttons = ReadButtons();
      
    switch (shift)
    {
    case 1:
    spincrement = 1.0;
    lcd.setCursor(10,1);
    lcd.print("x10");
    break;
    
    case 0:
    spincrement = 0.1;
    lcd.setCursor(10,1);
    lcd.print("   ");
    break;
    }
    
    switch (buttons)
    {
      case KEYPAD_SHIFT:
      if (shift == 0)
      {
        shift = 1;
      }
      else
      {
        shift = 0;
      }
      delay(200);
      break;

    case KEYPAD_RIGHT:
      opState = TUNE_P;
      break;

    case KEYPAD_LEFT:
      opState = OFF;
      break;  

    case KEYPAD_UP:
      Setpoint += spincrement;
      delay(200);
      break;

    case KEYPAD_DOWN:
      Setpoint -= spincrement;
      delay(200);
      break;
    }	  
    if (opState != SETP)
    {
      return;
    } 

    if ((millis() - lastInput) > 3000)  // return to RUN after 3 seconds idle
    {
      opState = RUN;
      return;
    }

    lcd.setCursor(0,1);
    lcd.print(Setpoint);
    lcd.print(" ");
    DoControl();
  }
}

// ************************************************
// Proportional Tuning State
// UP/DOWN to change Kp
// RIGHT for Ki
// LEFT for setpoint
// SHIFT for 10x tuning
// ************************************************
void TuneP()
{
  lcd.print(F("Set Kp"));
  int buttons = -1;
  while(true)
  {
    buttons = ReadButtons();
      
    switch (shift)
    {
    case 1:
    tpincrement = 10.0;
    lcd.setCursor(10,1);
    lcd.print("x10");
    break;
    
    case 0:
    tpincrement = 1;
    lcd.setCursor(10,1);
    lcd.print("   ");
    break;
    }

    switch (buttons)
    {
    case KEYPAD_SHIFT:
      if (shift == 0)
      {
        shift = 1;
      }
      else
      {
        shift = 0;
      }
      delay(200);
      break;

    case KEYPAD_LEFT:
      opState = SETP;
      break;

    case KEYPAD_RIGHT:
      opState = TUNE_I;
      break;

    case KEYPAD_UP:
      Kp += tpincrement;
      delay(200);
      break;

    case KEYPAD_DOWN:
      Kp -= tpincrement;
      delay(200);
      break;
    }

    if (opState != TUNE_P)
    {
      return;
    } 

    if ((millis() - lastInput) > 3000)  // return to RUN after 3 seconds idle
    {
      opState = RUN;
      return;
    }

    lcd.setCursor(0,1);
    lcd.print(Kp);
    lcd.print(" ");
    DoControl();
  }
}

// ************************************************
// Integral Tuning State
// UP/DOWN to change Ki
// RIGHT for Kd
// LEFT for Kp
// SHIFT for 10x tuning
// ************************************************
void TuneI()
{
  lcd.print(F("Set Ki"));
  int buttons = -1;
  while(true)
  {
    buttons = ReadButtons();

    switch (shift)
    {
    case 1:
    tiincrement = 0.1;
    lcd.setCursor(10,1);
    lcd.print("x10");
    break;
    
    case 0:
    tiincrement = 0.01;
    lcd.setCursor(10,1);
    lcd.print("   ");
    break;
    }
    
    switch (buttons)
    {

    case KEYPAD_SHIFT:
      if (shift == 0)
      {
        shift = 1;
      }
      else
      {
        shift = 0;
      }
      delay(200);
      break;

    case KEYPAD_LEFT:
      opState = TUNE_P;
      break;

    case KEYPAD_RIGHT:
      opState = TUNE_D;
      break;

    case KEYPAD_UP:
      Ki += tiincrement;
      delay(200);
      break;

    case KEYPAD_DOWN:
      Ki -= tiincrement;
      delay(200);
      break;
    }
    if (opState != TUNE_I)
    {
      return;
    } 

    if ((millis() - lastInput) > 3000)  // return to RUN after 3 seconds idle
    {
      opState = RUN;
      return;
    }

    lcd.setCursor(0,1);
    lcd.print(Ki);
    lcd.print(" ");
    DoControl();
  }
}

// ************************************************
// Derivative Tuning State
// UP/DOWN to change Kd
// RIGHT for setpoint
// LEFT for Ki
// SHIFT for 10x tuning
// ************************************************
void TuneD()
{
  lcd.print(F("Set Kd"));
  int buttons = -1;
  while(true)
  {
    buttons = ReadButtons();
    
    switch (shift)
    {
    case 1:
    tdincrement = 0.1;
    lcd.setCursor(10,1);
    lcd.print("x10");
    break;
    
    case 0:
    tdincrement = 0.01;
    lcd.setCursor(10,1);
    lcd.print("   ");
    break;
    }

    switch (buttons)
    {
    case KEYPAD_SHIFT:
      if (shift == 0)
      {
        shift = 1;
      }
      else
      {
        shift = 0;
      }
      delay(200);
      break;
    case KEYPAD_LEFT:
      opState = TUNE_I;
      break;
    case KEYPAD_RIGHT:
      opState = RUN;
      break;
    case KEYPAD_UP:
      Kd += tdincrement;
      delay(200);
      break;
    case KEYPAD_DOWN:
      Kd -= tdincrement;
      delay(200);
      break;
    }
    if (opState != TUNE_D)
    {
      return;
    }

    if ((millis() - lastInput) > 3000)  // return to RUN after 3 seconds idle
    {
      opState = RUN;
      return;
    }

    lcd.setCursor(0,1);
    lcd.print(Kd);
    lcd.print(" ");
    DoControl();
  }
}

// ************************************************
// PID COntrol State
// SHIFT and RIGHT for autotune
// RIGHT - Setpoint
// LEFT - OFF
// ************************************************
void Run()
{
  // set up the LCD's number of rows and columns: 
  lcd.print(F("Sp: "));
  lcd.print(Setpoint);
  lcd.write(1);
  lcd.print(F("C : "));

  SaveParameters();
  myPID.SetTunings(Kp,Ki,Kd);
  int buttons = -1;
  while(true)
  {
    setBacklight();  // set backlight based on state

    buttons = ReadButtons();
    switch (buttons)
    {
    case KEYPAD_SHIFT:
      if (shift == 0)
      {
        shift = 1;
      }
      else
      {
        shift = 0;
      }
      delay(200);
      break;

    case KEYPAD_RIGHT:
      if ((shift == 1)  
        && (abs(Input - Setpoint) < 0.5))  // Should be at steady-state
      {
        StartAutoTune();
      }
      else
      {
        opState = SETP;
      }
      break;

    case KEYPAD_LEFT:
      opState = OFF;
      break;
    }

    if (opState != RUN)
    {
      return;
    }

    DoControl();

    if (shift == 1)
    {
      lcd.setCursor(12,0);
      lcd.print("AT?");
    }
    else
    {
      lcd.setCursor(12,0);
      lcd.print("   ");
    }

    lcd.setCursor(0,1);
    lcd.print(Input);
    lcd.write(1);
    lcd.print(F("C : "));

    float pct = map(Output, 0, WindowSize, 0, 1000);
    lcd.setCursor(10,1);
    lcd.print(F("      "));
    lcd.setCursor(10,1);
    lcd.print(pct/10);
    //lcd.print(Output);
    lcd.print("%");

    lcd.setCursor(15,0);
    if (tuning)
    {
      lcd.print("T");
    }
    else
    {
      lcd.print(" ");
    }

    // periodically log to serial port in csv format
    if (millis() - lastLogTime > logInterval)  
    {
      Serial.print(Input);
      Serial.print(",");
      Serial.println(Output);
      lastLogTime = millis();  
  }
    
    delay(100);
  }
}

// ************************************************
// Execute the control loop
// ************************************************
void DoControl()
{
  // Read the input:
  if (sensors.isConversionAvailable(0))
  {
    Input = sensors.getTempC(tempSensor);
    sensors.requestTemperatures(); // prime the pump for the next one - but don't wait
  }

  if (tuning) // run the auto-tuner
  {
    if (aTune.Runtime()) // returns 'true' when done
    {
      FinishAutoTune();
    }
  }
  else // Execute control algorithm
  {
    myPID.Compute();
  }

  // Time Proportional relay state is updated regularly via timer interrupt.
  onTime = Output; 
}

// ************************************************
// Called by ISR every 15ms to drive the output
// ************************************************
void DriveOutput()
{  
  long now = millis();
  // Set the output
  // "on time" is proportional to the PID output
  if(now - windowStartTime>WindowSize)
  { //time to shift the Relay Window
    windowStartTime += WindowSize;
  }
  if((onTime > 100) && (onTime > (now - windowStartTime)))
  {
    digitalWrite(RelayPin,HIGH);
  }
  else
  {
    digitalWrite(RelayPin,LOW);
  }
}

// ************************************************
// Set Backlight based on the state of control
// ************************************************
void setBacklight()
{
  unsigned long currentMillis = millis(); 
  if(currentMillis - previousMillis > interval) {
    // save the last time you blinked the LED 
    previousMillis = currentMillis;   

    if (ledState == LOW)
      ledState = HIGH;
    else
      ledState = LOW;
  }
  if (tuning)
  {
    digitalWrite(RedPin,ledState); // Tuning Mode
    digitalWrite(YellowPin,ledState); // Tuning Mode
    digitalWrite(GreenPin,ledState); // Tuning Mode
  }
  else if (abs(Input - Setpoint) > 1.0)  
  {
    digitalWrite(RedPin,ledState); 
    digitalWrite(YellowPin,HIGH); 
    digitalWrite(GreenPin,HIGH);  // High Alarm - off by more than 1 degree
  }
  else if (abs(Input - Setpoint) > 0.2)  
  {
    digitalWrite(RedPin,LOW);
    digitalWrite(YellowPin,ledState); 
    digitalWrite(GreenPin,HIGH);  // Low Alarm - off by more than 0.2 degrees
  }
  else
  {
    digitalWrite(RedPin,LOW);
    digitalWrite(YellowPin,LOW); 
    digitalWrite(GreenPin,ledState);   // We're on target!
  }
}

// ************************************************
// Start the Auto-Tuning cycle
// ************************************************

void StartAutoTune()
{
  // REmember the mode we were in
  ATuneModeRemember = myPID.GetMode();

  // set up the auto-tune parameters
  aTune.SetNoiseBand(aTuneNoise);
  aTune.SetOutputStep(aTuneStep);
  aTune.SetLookbackSec((int)aTuneLookBack);
  tuning = true;
}

// ************************************************
// Return to normal control
// ************************************************
void FinishAutoTune()
{
  tuning = false;

  // Extract the auto-tune calculated parameters
  Kp = aTune.GetKp();
  Ki = aTune.GetKi();
  Kd = aTune.GetKd();

  // Re-tune the PID and revert to normal control mode
  myPID.SetTunings(Kp,Ki,Kd);
  myPID.SetMode(ATuneModeRemember);

  // Persist any changed parameters to EEPROM
  SaveParameters();
}

// ************************************************
// Check buttons and time-stamp the last press
// ************************************************
int ReadButtons()
{
  int buttons = lcd.button();
  if (buttons != KEYPAD_NONE)
  {
    lastInput = millis();
  }
  return buttons;
}

// ************************************************
// Save any parameter changes to EEPROM
// ************************************************
void SaveParameters()
{
  if (Setpoint != EEPROM_readDouble(SpAddress))
  {
    EEPROM_writeDouble(SpAddress, Setpoint);
  }
  if (Kp != EEPROM_readDouble(KpAddress))
  {
    EEPROM_writeDouble(KpAddress, Kp);
  }
  if (Ki != EEPROM_readDouble(KiAddress))
  {
    EEPROM_writeDouble(KiAddress, Ki);
  }
  if (Kd != EEPROM_readDouble(KdAddress))
  {
    EEPROM_writeDouble(KdAddress, Kd);
  }
}

// ************************************************
// Load parameters from EEPROM
// ************************************************
void LoadParameters()
{
  // Load from EEPROM
  Setpoint = EEPROM_readDouble(SpAddress);
  Kp = EEPROM_readDouble(KpAddress);
  Ki = EEPROM_readDouble(KiAddress);
  Kd = EEPROM_readDouble(KdAddress);

  // Use defaults if EEPROM values are invalid
  if (isnan(Setpoint))
  {
    Setpoint = 60;
  }
  if (isnan(Kp))
  {
    Kp = 850;
  }
  if (isnan(Ki))
  {
    Ki = 0.5;
  }
  if (isnan(Kd))
  {
    Kd = 0.1;
  }  
}


// ************************************************
// Write floating point values to EEPROM
// ************************************************
void EEPROM_writeDouble(int address, double value)
{
  byte* p = (byte*)(void*)&value;
  for (int i = 0; i < sizeof(value); i++)
  {
    EEPROM.write(address++, *p++);
  }
}

// ************************************************
// Read floating point values from EEPROM
// ************************************************
double EEPROM_readDouble(int address)
{
  double value = 0.0;
  byte* p = (byte*)(void*)&value;
  for (int i = 0; i < sizeof(value); i++)
  {
    *p++ = EEPROM.read(address++);
  }
  return value;
}
