/*
# t400-firmware

## Overview
Firmware for the Pax Instruments T400 temperature datalogger

## Setup
1. Install the Arduino IDE from http://arduino.cc/en/Main/Software. Use version 1.6.7
2. Install the following Arduino libraries.
  - U8Glib graphical LCD https://github.com/PaxInstruments/u8glib
  - MCP3424 ADC https://github.com/PaxInstruments/MCP3424
  - MCP980X temperature sensor https://github.com/PaxInstruments/MCP980X
  - DS3231 RTC https://github.com/PaxInstruments/ds3231
  - SdFat library https://github.com/PaxInstruments/SdFat (use the SdFat directory within this repository)
3. Install the Pax Instruments hardware core (unzip it and move it to the hardware/ directory in your Sketches folder):
  - https://github.com/PaxInstruments/ATmega32U4-bootloader
4. Restart Arduino if it was already running
5. In tools->board, set the Arduino board to "Pax Instruments T400".
6. In tools->port, select the serial port corresponding to the T400 (Arduino should identify it correctly)

*/


// Import libraries
#include "t400.h"             // Board definitions

#include <Wire.h>             // I2C
#include <SPI.h>

#include "PaxInstruments-U8glib.h"  // LCD
#include <MCP3424.h>          // ADC
#include <MCP980X.h>          // Ambient/junction temperature sensor
#include <ds3231.h>           // RTC
#include "power.h"            // Manage board power
#include "buttons.h"          // User buttons
#include "typek_constant.h"   // Thermocouple calibration table
#include "functions.h"        // Misc. functions
#include "sd_log.h"           // SD card utilities

#define BUFF_MAX         80   // Size of the character buffer

char fileName[] =        "LD0001.CSV";

// MCP3424 for thermocouple measurements
MCP3424 thermocoupleAdc(MCP3424_ADDR, MCP342X_GAIN_X8, MCP342X_16_BIT);  // address, gain, resolution

MCP980X ambientSensor(0);      // Ambient temperature sensor

// Map of ADC inputs to thermocouple channels
const uint8_t temperatureChannels[SENSOR_COUNT] = {1, 0, 3, 2};

int16_t temperatures_int[SENSOR_COUNT] = {OUT_OF_RANGE_INT,OUT_OF_RANGE_INT,
                                          OUT_OF_RANGE_INT,OUT_OF_RANGE_INT};

// Ambient temperature
int16_t ambient =  0;

boolean backlightEnabled = true;

// Available log intervals, in seconds
#define LOG_INTERVAL_COUNT 6
const uint8_t logIntervals[LOG_INTERVAL_COUNT] = {1, 2, 5, 10, 30, 60};


uint8_t logInterval    = 0; // currently selected log interval
boolean logging = false;    // True if we are currently logging to a file


bool timeToSample = false;  // If true, the display should be redrawn
uint8_t isrTick = 0;        // Number of 1-second tics that have elapsed since the last sample
uint8_t lastIsrTick = 0;    // Last tick that we redrew the screen
uint32_t logTimeSeconds;    // Number of seconds that have elapsed since logging began

struct ts rtcTime;          // Buffer to read RTC time into

uint8_t temperatureUnit;    // Measurement unit for temperature

uint8_t graphChannel = 4;


void rotateTemperatureUnit() {
  // Rotate the unit
  temperatureUnit = (temperatureUnit + 1) % TEMPERATURE_UNITS_COUNT;

  // Reset the graph so we don't have to worry about scaling it
  Display::resetGraph();

  // TODO: Convert the current data to new units?
  return;
}

// Convert temperature from celcius to the new unit
double convertTemperature(double Celcius) {
  if(temperatureUnit == TEMPERATURE_UNITS_C) {
    return Celcius;
  }
  else if(temperatureUnit == TEMPERATURE_UNITS_K) {
    return Celcius + 273.15;
  }
  // TEMPERATURE_UNITS_F
  return 9.0/5.0*Celcius+32;
}

// This function runs once. Use it for setting up the program state.
void setup(void) {
  uint8_t x;

  Power::setup();
  ChargeStatus::setup();
  #if SERIAL_OUTPUT_ENABLED
  Serial.begin(9600);
  #endif

  Wire.begin(); // Start using the Wire library; does the i2c communication.

  Backlight::setup();
  Backlight::set(backlightEnabled);

  Display::setup();
  Display::resetGraph();

  thermocoupleAdc.begin();

  ambientSensor.begin();
  ambientSensor.writeConfig(ADC_RES_12BITS);

  // Set up the RTC to generate a 1 Hz signal
  pinMode(RTC_INT, INPUT);
  DS3231_init(0);

  // And configure the atmega to interrupt on falling edge of the 1 Hz signal
  EICRA |= _BV(ISC21);    // Configure INT2 to trigger on falling edge
  EIMSK |= _BV(INT2);    // and enable the INT2 interrupt

  Buttons::setup();

  return;
}

void startLogging() {

  if(logging) return;

  sd::init();
  logging = sd::open(fileName);
  return;
}

void stopLogging() {
  if(!logging) return;

  logging = false;
  sd::close();
  return;
}

static void readTemperatures() {
  int32_t measuredVoltageUv;
  int32_t compensatedVoltage;

  float tempflt;
  int16_t tempint=0;

/******************* Float Math Start ********************/
  tempflt = ambientSensor.readTempC16(AMBIENT) / 16.0;  // Read ambient temperature in C
  ambient = ((int16_t)(tempflt*10));
/******************* Float Math End ********************/

  // ADC read loop: Start a measurement, wait until it is finished, then record it
  for(uint8_t channel = 0; channel < SENSOR_COUNT; channel++)
  {

    // TODO: There has to be a better way
    thermocoupleAdc.startMeasurement(temperatureChannels[channel]);
    do {
      // Delay a while. At 16-bit resolution, the ADC can do a speed of 1/15 = .066seconds/cycle
      // Let's wait a little longer than that in case there is set up time for changing channels.
      delay(70);
    } while(!thermocoupleAdc.measurementReady());

/******************* Float Math Start ********************/
    // This gets a float value.  We do a y = mx+b for calibration
    measuredVoltageUv = thermocoupleAdc.getMeasurementUv() * MCP3424_CALIBRATION_MULTIPLY + MCP3424_CALIBRATION_ADD; // Calibration value: MCP3424_OFFSET_CALIBRATION
    compensatedVoltage = measuredVoltageUv + GetJunctionVoltage( (((float)(ambient))/10.0) );
    tempflt = GetTypKTemp(compensatedVoltage);
    if(tempflt != OUT_OF_RANGE)
    {
      //temperature = convertTemperature(temperature + ambient_float);
      tempflt = convertTemperature(tempflt);
    }
    tempint = ((int16_t)(tempflt*10));
    temperatures_int[channel] = tempint;

/******************* Float Math End ********************/

    // DEBUG: Fake some data
    #if 1
    temperatures_int[0] = 123;
    temperatures_int[1] = 345;
    temperatures_int[2] = OUT_OF_RANGE_INT;
    temperatures_int[3] = OUT_OF_RANGE_INT;
    #endif

  } // end for loop

  return;
}

static void writeOutputs() {

  static char updateBuffer[BUFF_MAX];      // Scratch buffer to write serial/sd output into
  uint8_t index=0;

  index += sprintf(&(updateBuffer[index]),"%d",logTimeSeconds);

  for(uint8_t i = 0; i < SENSOR_COUNT; i++)
  {
    if(temperatures_int[i] == OUT_OF_RANGE_INT)
    {
        index += sprintf(&(updateBuffer[index]), ", -");
    }else {
      index+=sprintf(&(updateBuffer[index]), ", %d.%d",temperatures_int[i]/10,temperatures_int[i]%10);
    }
  }

  #if SERIAL_OUTPUT_ENABLED
  Serial.println(updateBuffer);
  #endif

  if(logging) {
    logging = sd::log(updateBuffer);
  }
  return;
}

// Reset the tick counter, so that a new measurement takes place within 1 second
void resetTicks() {
  noInterrupts();
  isrTick = logIntervals[logInterval]-1; // TODO: This is a magic number
  logTimeSeconds = 0;
  interrupts();
  return;
}

// This function is called periodically, and performs slow tasks:
// Taking measurements
// Updating the screen
void loop() {
  bool needsRefresh = false;  // If true, the display needs to be updated

  if(timeToSample) {
    timeToSample = false;

    readTemperatures();

    //DS3231_get(&rtcTime);

    writeOutputs();
    Display::updateGraphData(temperatures_int);
    Display::updateGraphScaling();

    needsRefresh = true;
  }

  // Check for button presses
  if(Buttons::pending()) {
    uint8_t button = Buttons::getPending();

    switch(button){
    case BUTTON_POWER:
      // Disable power
      if(!logging) {
        Display::clear();
        Backlight::set(0);
        Power::shutdown();
      }
      break;

    case BUTTON_A:
      // Start/stop logging
      #if SD_LOGGING_ENABLED
      // NOTE: Logging takes up 30% of the flash!!!
      if(!logging) {
        startLogging();
      }
      else {
        stopLogging();
      }
      resetTicks();
      needsRefresh = true;
      #endif
      break;

    case BUTTON_B:
      // Cycle log interval
      if(!logging) {
        logInterval = (logInterval + 1) % LOG_INTERVAL_COUNT;
        resetTicks();

        Display::resetGraph();  // Reset the graph, to keep the x axis consistent
        resetTicks();
        needsRefresh = true;
      }
      break;
    case BUTTON_C:
      // Cycle temperature units
      if(!logging) {
        rotateTemperatureUnit();
        resetTicks();
        needsRefresh = true;
      }
      break;
    case BUTTON_D:
      // Sensor display mode
      graphChannel = (graphChannel + 1) % GRAPH_CHANNELS_COUNT;
      while(graphChannel < 4 & temperatures_int[graphChannel] == OUT_OF_RANGE_INT) {
        graphChannel = (graphChannel + 1) % GRAPH_CHANNELS_COUNT;
      }
      needsRefresh = true;
      break;
    case BUTTON_E:
      // Toggle backlight
      backlightEnabled = !backlightEnabled;
      Backlight::set(backlightEnabled);
      break;

    default: break;
    } // end button select

  } // end if button pending

  // If we are charging, refresh the display every second to make the charging animation
  if(ChargeStatus::get() == ChargeStatus::CHARGING) {
    if(lastIsrTick != isrTick) {
      needsRefresh = true;
      lastIsrTick = isrTick;
    }
  }

  // Draw the display
  if(needsRefresh)
  {
    char * ptr = NULL;
    if(logging) ptr = fileName;

    Display::draw(
      temperatures_int,
      graphChannel,
      temperatureUnit,
      ptr,
      logIntervals[logInterval],
      ChargeStatus::get(),
      ChargeStatus::getBatteryLevel()
    );

  }

  // Sleep if we are on battery power
  // Note: Don't sleep if there is power, in case we need to communicate over USB
  if(ChargeStatus::get() == ChargeStatus::DISCHARGING) {
    Power::sleep();
  }

  return;
}


// 1 Hz interrupt from RTC
ISR(INT2_vect)
{
  isrTick = (isrTick + 1)%(logIntervals[logInterval]);
  logTimeSeconds++;

  if(isrTick == 0) {
    timeToSample = true;
  }
  return;
}
