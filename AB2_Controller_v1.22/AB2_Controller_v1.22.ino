//2022-2023 (c) Jacob Conde All Rights Reserved. Do Not Distribute. Absolutely Proprietary.

/*1.2 Changelog
 * 12/16/22 - Added Handshake for drink timing data from PC
 * 1.21 - 12/19/22 - Refactored temperature and fan checks in the idle loop to their own static functions
 *                 - Added newline characters after serial messages intended for the PC, 
 *                 continue doing so in the future
 *      - 12/22/22 - Added function updateActivePumpLCD which prints the pumpID/servoID of the active device to the
 *                 bottom row of the debug LCD. Only used within the dispenseLiquid function
 *                 Potential implications for precise timing of pump devices if LCD calls take too long?
 * 1.22 - 12/24/22 - Implemented jog mode
 *      - 12/25/22 - Improved formatting on updateActivePumpLCD with switch statement
 *      - 12/26/22 - Added updateActivePumpLCD to jog mode
 *                 
*/

//TODO
//just use another L298N for the fan controller dummy
  //connect the reverse direction pin to ground so it can't drive in reverse (breaks fan? TBD)
//Refactor Serial buffer check/handshake into separate function, and support multiple different commands from the
  //PC, like standard drink timing mode and support for a PC activated Jog mode.
//IDEALLY jog mode is done with combinational logic and/or a multiplexer, so only 3 data pins would be needed
  //instead of 10

#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DallasTemperature.h>
#include <OneWire.h>

//Constants
#define BAUD_RATE 9600 //baud rate for serial transmission
#define NUMBER_OF_PUMPS 6 //total number of pumps for which Serial commands are sent
#define NUMBER_OF_SERVOS 4 //total number of bottle servos for which Serial commands are sent
#define SLOW_SPEED_RATE 160 //for analogWrite on L298N Enable pin, value range from 0:255
#define FULL_SERVO_ANGLE 0 //the angle, from 0 to 180, of the servo for a fully open valve
#define CLOSED_SERVO_ANGLE 120 //servo angle for which the pinch valve is fully closed
#define HANDSHAKE_RX '?' //used to indicate that the PC has drink data ready
#define HANDSHAKE_TX "!!!!!!!!" //used to indicate to the PC that the arduino is ready to recieve drink data

//the following still need to be determined experimentally
#define SLOW_SERVO_ANGLE 90 //servo angle for which there is a reduced flow rate
#define SLOW_1ML_RATE 250 //how long it takes to pump 1mL using the slow continuous speed


//Pin assignments
#define ONE_WIRE_BUS 53 //Data pin on the temp probe
#define fanTransistorPin 52 //Controls the gate of mosfet to switch on the fan

//Jog mode pins
#define jogEnable 34
#define jogLED 45 //of questionable utiltity, LED uses power from enable pin?
#define jogP1 35
#define jogP2 36
#define jogP3 37
#define jogP4 38
#define jogP5 39
#define jogP6 40
#define jogS1 41
#define jogS2 42
#define jogS3 43
#define jogS4 44

//assigning the peristaltic pump pin numbers to their systematic names
//eg. b12 - 2nd(b) L298N board, 1st pump, 2nd input, E indicates Enable pin
#define a1E 2
#define a2E 3
#define a11 22
#define a12 23
#define a21 24
#define a22 25

#define b1E 4
#define b2E 5
#define b11 26
#define b12 27
#define b21 28
#define b22 29

#define c1E 6
#define c2E 7
#define c11 30
#define c12 31
#define c21 32
#define c22 33

//define servo pins here (pwm pins)
#define s1 8
#define s2 9
#define s3 10
#define s4 11

//the array of all pump pins is used when defining using pinMode(OUTPUT)
int pumpPins[] = {a1E, a2E, a11, a12, a21, a22, b1E, b2E, b11, b12, b21, b22, c1E, c2E, c11, c12, c21, c22};
int servoPins[] = {s1, s2, s3, s4};
byte jogPumpPins[] = {jogP1, jogP2, jogP3, jogP4, jogP5, jogP6};
byte jogServoPins[] = {jogS1, jogS2, jogS3, jogS4};

//peristaltic pump pin arrays
int pump1[] = {a1E, a11, a12};
int pump2[] = {a2E, a21, a22};
int pump3[] = {b1E, b11, b12};
int pump4[] = {b2E, b21, b22};
int pump5[] = {c1E, c11, c12};
int pump6[] = {c2E, c21, c22};
int* allPumps[] = {pump1, pump2, pump3, pump4, pump5, pump6};

//Create the servo objects
Servo servo1;
Servo servo2;
Servo servo3;
Servo servo4;

//Servo array
Servo allServos[] = {servo1, servo2, servo3, servo4};

//Initialize data arrays, these should be global (well maybe not 'should' but they're gonna be)
//TODO: move these to static variables 
int pumpIDs[NUMBER_OF_PUMPS];
long pumpFastSeconds[NUMBER_OF_PUMPS];
long pumpSlowSeconds[NUMBER_OF_PUMPS];
int pumpCycles[NUMBER_OF_PUMPS];
int servoIDs[NUMBER_OF_SERVOS];
long servoFastSeconds[NUMBER_OF_SERVOS];
long servoSlowSeconds[NUMBER_OF_SERVOS];

//Other Global Variables
//TODO: refactor to an error printing function, errorcounter is static var, function arg is message
int errorcounter = 0;

//Initialize the LCD
LiquidCrystal_I2C lcd(0x3F, 20, 4);

//Initialize the temperature probe communication
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);


/*
 * Setup Function
 */


void setup() {
  //Open up the serial port
  Serial.begin(BAUD_RATE);
  while(!Serial){
    ;
  }

  //Set the pump and servo pins to pinMode(OUTPUT)
  for (int i = 0; i < sizeof(pumpPins)/2; i++) { //sizeof returns the size in bytes, each int is 2 bytes
    pinMode(pumpPins[i], OUTPUT);
  }

  //Initialize jog pins
  for(int i = 0; i < sizeof(jogPumpPins); i++){
    pinMode(jogPumpPins[i], INPUT);
  }
  for(int i = 0; i < sizeof(jogServoPins); i++){
    pinMode(jogServoPins[i], INPUT);
  }
  pinMode(jogEnable, INPUT);
  //Possibly Redundant
  pinMode(jogLED, OUTPUT);

  //Initialize the servo pins
  servo1.attach(s1);
  servo2.attach(s2);
  servo3.attach(s3);
  servo4.attach(s4);

  //Ensure all of the servos are in the closed position;
  stopAllServos();

  //Initialize Jog mode pins

  //LCD Message Formatting
  lcd.init();
  lcd.backlight();
  //Line 0: Temperature
  //Line 1: Status
  //Line 2: Errors
  //Line 3: Active Pumps & Servos
  lcd.setCursor(0,0);
  lcd.print("Temp: ");// start temp. value printing at lcd.setCursor(6,0)
  lcd.setCursor(11,0);
  lcd.print("C");
  lcd.setCursor(13,0);
  lcd.print("Fan:");
  //lcd.setCursr(18,0);
  //lcd.print("F");
  lcd.setCursor(0,1);
  lcd.print("Step: ");// start status printing at lcd.setCursor(6,1) 
  lcd.setCursor(0,2);
  lcd.print("Err: ");// start at lcd.setCursor(5,2)

  //Initialize the DS18B20 temperature probe
  sensors.begin();
  pinMode(fanTransistorPin, OUTPUT);
  digitalWrite(fanTransistorPin, HIGH);
  //Waste three quarters of a second blasting the fans just to show off
  delay(750);
  digitalWrite(fanTransistorPin, LOW);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//Pumping and Servoing Functions
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

//Turn on the L298N pumps at full speed, pumpIDs start at 1 (not 0 indexed like arrays)
//Pump rate: 6.6 mL/s, 151.5ms/mL
void pumpOnFull(int pumpID){
  if (pumpID <= 0 || pumpID > NUMBER_OF_PUMPS){
    Serial.println("pumpOnFull: pumpID out of range");
  }
  else if(pumpID > 0 && pumpID <= NUMBER_OF_PUMPS) {
    digitalWrite(allPumps[pumpID-1][0], HIGH);
    digitalWrite(allPumps[pumpID-1][1], HIGH);
    digitalWrite(allPumps[pumpID-1][2], LOW);
  }
  else {
    Serial.println("pumpOnFull: Invalid pumpID");
  }
}


//Turn on L298N pumps at slowest continuous speed
//Pump rate 4mL/s, 250ms/mL
void pumpOnSlow(int pumpID){
  if (pumpID <= 0 || pumpID > NUMBER_OF_PUMPS){
    Serial.println("pumpOff: pumpID out of range");
  }
  else if(pumpID > 0 && pumpID <= NUMBER_OF_PUMPS) {
    analogWrite(allPumps[pumpID-1][0], SLOW_SPEED_RATE);
    digitalWrite(allPumps[pumpID-1][1], HIGH);
    digitalWrite(allPumps[pumpID-1][2], LOW);
  }
  else {
    Serial.println("pumpOff: Invalid pumpID");
  }
}

//THIS FUNCTION should be DEPRECATED, and the intermittent pumping idea abandoned entirely tbh
//Pump a small amount of liquid, 1 mL? alt func. names: pump1mL
//Pump rate 1mL per cycle, 1 cycle every 500ms?
//THIS WONT WORK BECAUSE IT WILL BE STUCK IN THIS FUNCTION FOR THE DURATION INSTEAD OF LOOPING IN THE MAIN LOOP
  //Change to just pump 1 mL and do the looping/waiting int the main loop 
void pumpIntermittent(int pumpID, int cycles){
  
  if (pumpID <= 0 || pumpID > NUMBER_OF_PUMPS){
    Serial.println("pumpIntermittent: pumpID out of range");
  }
  else if(pumpID > 0 && pumpID <= NUMBER_OF_PUMPS) {
    ;
  }
  else {
    Serial.println("pumpIntermittent: Invalid pumpID");
  }
}

//Turn off a specified pump
void pumpOff(int pumpID){
  if (pumpID <= 0 || pumpID > NUMBER_OF_PUMPS){
    Serial.println("pumpOff: pumpID out of range");
  }
  else if(pumpID > 0 && pumpID <= NUMBER_OF_PUMPS) {
    digitalWrite(allPumps[pumpID-1][0], LOW);
    digitalWrite(allPumps[pumpID-1][1], LOW);
    digitalWrite(allPumps[pumpID-1][2], LOW);
  }
  else {
    Serial.println("pumpOff: Invalid pumpID");
  }
}

//Fully open the bottle servo
void servoOnFull(int servoID){
  allServos[servoID - 1].write(FULL_SERVO_ANGLE);
}

//Open the bottle servo to a reduced degree
void servoOnSlow(int servoID){
  allServos[servoID - 1].write(SLOW_SERVO_ANGLE);
}

//Turn off a specified servo
void servoOff(int servoID){
  allServos[servoID - 1].write(CLOSED_SERVO_ANGLE);
}

//quick Stop function that just sets all pump pins to low by iterating though the master pump pin array
void stopAllPumps(){
  for(int i = 0; i < 17; i++){
    digitalWrite(pumpPins[i], LOW);
  }
}

//quick stop function that closes all servos
void stopAllServos(){
  for(int i = 0; i < NUMBER_OF_SERVOS; i++) {
    allServos[i].write(CLOSED_SERVO_ANGLE);
  }
}

//Parse serial data containing the pump timings, and store those values in the global arrays
//Returns true iff the terminator character was seen when expected, returns false otherwise
boolean receiveSerialData() {
  
  boolean transmissionStatus = false; //default return value
  int initiatorTimeout = 0;
  
  if (Serial.available()) {
    //delay(100); // Small delay to ensure that serial communication is complete
    //Since sending the times in ms the message is larger than the Serial buffer
    //however, since the baud rate is relatively low, we can start to clear the buffer
    //in time so that it doesn't fill up!
    //At 9600 baud, one byte is received every 1.04 ms
    delay(10); //new smaller delay just to be safe

    while(Serial.peek() != '<'){
      Serial.read(); //discard any data before the initiator symbol
      delay(1);
      initiatorTimeout++; //increment timeout check
      if(initiatorTimeout > 1000){
        errorcounter++;
        lcd.setCursor(18,2);
        lcd.print(errorcounter);
        break;
      }
    }
    
    if(Serial.read() == '<'){
      unsigned long startTimeTransmission = millis();
      unsigned long elapsedTimeTransmission;
      Serial.println("Initiator Seen");
      //lcd.setCursor(0,0);
      //lcd.print("Init Seen");
      
      for (int i = 0; i < NUMBER_OF_PUMPS; i++){
        String pump = Serial.readStringUntil(':');//YOU MUST USE SINGLE QUOTES HERE MUST HAVE TO HAVE TO ALWAYS
        String pumpFastSec = Serial.readStringUntil(':');
        String pumpSlowSec = Serial.readStringUntil(':');
        String pumpCyc = Serial.readStringUntil('&');
        //if(Serial.peek() == -1){Serial.println("EOB");}
        if(Serial.peek() == '>'){
          while(Serial.available()){Serial.read();}//If the '>' terminator is reached, clear the serial buffer
          elapsedTimeTransmission = millis() - startTimeTransmission;
        }
        else if(Serial.peek() == '|'){
          Serial.println("moving on to servo data");
          //lcd.setCursor(0,1);
          //lcd.print("pipe seen");
        }
        else {
          Serial.println("terminator not seen, pump");
          //if you don't see the terminator, assume invalid string
          //clear the arrays, and request another string? todo
        }
  
        //Cast the String number to int, store in their respective arrays
        pumpIDs[i] = pump.toInt();
        pumpFastSeconds[i] = pumpFastSec.toInt();
        pumpSlowSeconds[i] = pumpSlowSec.toInt();
        pumpCycles[i] = pumpCyc.toInt();
      }//end for loop


      if (Serial.peek() == '|'){
        Serial.readStringUntil('|'); //remove the '|' delimiter from the Serial buffer
        for (int i = 0; i < NUMBER_OF_SERVOS; i++){
          String servo = Serial.readStringUntil(':');
          String servoFastSec = Serial.readStringUntil(':');
          String servoSlowSec = Serial.readStringUntil('&');
          if(Serial.peek() == '>'){
            //Serial.println("Terminator seen, clearing the serial buffer");
              //for some reason adding this breaks it???
            while(Serial.available()){Serial.read();}//If the '>' terminator is reached, clear the serial buffer
            elapsedTimeTransmission = millis() - startTimeTransmission;
            
            //At this point we can assume that data was of the expected format
            transmissionStatus = true;
          }
          else {
            Serial.println("terminator not seen, servo");
            //if you don't see the terminator, assume invalid string
            //clear the arrays, and request another string? todo
          }
          
          servoIDs[i] = servo.toInt();
          servoFastSeconds[i] = servoFastSec.toInt();
          servoSlowSeconds[i] = servoSlowSec.toInt();
        }
      }
      else if (Serial.peek() == '>'){
        Serial.println("String terminator seen: no servo data read");
        while(Serial.available()){Serial.read();} //clear the serial buffer
      }
      else {
        Serial.println("Invalid String format: no terminator or servo delimiter?");
        //while(Serial.available()){Serial.read();} //clear the serial buffer
      }

        //Pump printouts
        Serial.println("Pumps: ");
        for(int n = 0; n < NUMBER_OF_PUMPS; n++){
          Serial.println(pumpIDs[n]);
        }
        Serial.println("Pump Fast Seconds: ");
        for(int n = 0; n < NUMBER_OF_PUMPS; n++){
          Serial.println(pumpFastSeconds[n]);
        }
        Serial.println("Pump Slow Seconds: ");
        for(int n = 0; n < NUMBER_OF_PUMPS; n++){
          Serial.println(pumpSlowSeconds[n]);
        }
        Serial.println("Intermittent Cycles: ");
        for(int n = 0; n < NUMBER_OF_PUMPS; n++){
          Serial.println(pumpCycles[n]);
        }

        //Servo Printouts
        Serial.println("Servos: ");
        for(int n = 0; n < NUMBER_OF_SERVOS; n++){
          Serial.println(servoIDs[n]);
        }
        Serial.println("Servo Fast Seconds: ");
        for(int n = 0; n < NUMBER_OF_SERVOS; n++){
          Serial.println(servoFastSeconds[n]);
        }
        Serial.println("Servo Slow Seconds: ");
        for(int n = 0; n < NUMBER_OF_SERVOS; n++){
          Serial.println(servoSlowSeconds[n]);
        }
        
        Serial.print("Transmission time: ");
        Serial.print(elapsedTimeTransmission);
        Serial.println(" ms");
    }
    else{
      Serial.println("Invalid String (no initiator)");
      while(Serial.available()){Serial.read();}//clear the serial buffer
    }     
  }
  return transmissionStatus;
}

//This is the main pumping sequnce
boolean dispenseLiquid() {
  
  boolean allDone = false; //Return Variable

  //Basic pumping and servoing
  boolean pumpsDone[] = {false, false, false, false, false, false};
  boolean servosDone[] = {false, false, false, false};
  boolean pumpOnSlowCheck[] = {false, false, false, false, false, false};
  boolean allPumpsDone = false;
  boolean allServosDone = false;
  unsigned long startTime = millis();
  unsigned long currentTime = millis();
  unsigned long elapsedTime = 0;
  unsigned long cycleStartTime;
  unsigned long decrementTime;

  //Intermittent pumping variables
  boolean isDeadTime = false;
  boolean startIntermittentCheck = false;
  int pumpLiveCycleTime[] = {0,0,0,0,0,0};
  int pumpDeadCycleTime[] = {0,0,0,0,0,0};
  unsigned long cycTimeAccum = 0;
  
  //debug things
  long dLLoops = 0;

  //convert the integer number of cycles to cycle times
//  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
//    pumpLiveCycleTime[i] = pumpCycles[i]*250;//250 ms/mL on slow
//    pumpDeadCycleTime[i] = pumpCycles[i]*750;//750 adds to 1000 so 1mL/s rate
//  }

  //Print out all pumps to the debug LCD, to be turned off as they are recognized as done later
  for(int pump = 0; pump < NUMBER_OF_PUMPS; pump++){
    updateActivePumpLCD(pump+1, pumpsDone[pump]);
  }
  for(int serv = 0; serv < NUMBER_OF_SERVOS; serv++){
    updateActivePumpLCD(serv+1+NUMBER_OF_PUMPS, servosDone[serv]);
  }
  
  while(!allDone){
    dLLoops++;
    cycleStartTime = millis();
    
    //check for a cancel signal
    //if (digitalRead(pin, HIGH) {
    //  break;
    //}
    //Maybe just have the PC send a few stop characters
    //if (Serial.read() == '!'){
    //  break;
    //}
    
    //Turning on pumps
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {

      //if the pump is Done, don't even bother with it
      if (pumpsDone[i] == true) {
        continue;
      }

      
      //Fast Pumping
      if (pumpFastSeconds[i] > 0) { //this structure implicitly schedules fast pumping first in all scenarios
        pumpOnFull(i+1);//pumpIDs start at 1, `1 more than the 0 indexed array and for loop
        //Serial.print("Pump on fast: ");
        //Serial.println(i+1);
        //Serial.println(millis() - startTime);
      }
      //Slow pumping
      else if (pumpFastSeconds[i] <= 0 && pumpSlowSeconds[i] > 0) {
        if (pumpOnSlowCheck[i] == false) {
          pumpOnSlow(i+1);
          pumpOnSlowCheck[i] = true;
        }
        //Serial.print("Pump on slow: ");
        //Serial.println(i+1);
        //Serial.println(millis() - startTime);
      }
      //Intermittent Cycles
      //current issue: still too slow when multiple other pumps are in play and running
      
//      else if (pumpFastSeconds[i] <= 0 && pumpSlowSeconds[i] <= 0 && pumpLiveCycleTime[i] > 0){
//
//        if (isDeadTime == false){
//          if (cycTimeAccum < 250 && pumpOnSlowCheck[i] == false){
//            pumpOnSlow(i+1);
//            pumpOnSlowCheck[i] ==  true;
//            Serial.print(" Live: "); //THIS shit started working only with the print statement
//            Serial.println(cycTimeAccum); //you gotta keep 'em in
//            //Does not work with more pumps running in the loop...
//          }
//          else if (cycTimeAccum >= 250){ //switch from live to dead
//            pumpOff(i+1);
//            pumpOnSlowCheck[i] = false;
//            isDeadTime = true;
//            cycTimeAccum = 0;
//          }
//        }
//        else if(isDeadTime == true){
//           if (cycTimeAccum < 750 && pumpOnSlowCheck[i] == true){
//             pumpOff(i+1);
//             pumpOnSlowCheck[i] = false;
//             Serial.print(" Dead: ");
//             Serial.println(cycTimeAccum);
//           }
//           else if (cycTimeAccum >= 750){ //switch from dead to live
//             pumpOnSlow(i+1);
//             pumpOnSlowCheck[i] = true;
//             isDeadTime = false;
//             cycTimeAccum = 0;
//           }
//        }
//      }
      
    }

    //Turning on servos
    for (int i = 0; i < NUMBER_OF_SERVOS; i++) {

      //If the servo is done, you don't need to go any further
      if (servosDone[i] == true){
        continue;
      }
      
      if (servoFastSeconds[i] > 0) {
        servoOnFull(i+1);
        Serial.print("Servo on fast: ");
        Serial.println(servoIDs[i]);
        Serial.println(millis()-startTime);
      }
      else if (servoFastSeconds[i] <= 0 && servoSlowSeconds[i] > 0) {
        servoOnSlow(i+1);
        Serial.print("Servo on slow: ");
        Serial.println(servoIDs[i]);
        Serial.println(millis()-startTime);
      }
    }


    //check the time, decrement that from the pump arrays
    //currentTime = millis();
    //decrementTime = currentTime - cycleStartTime;
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {

      if (pumpsDone[i] == true){
        continue;
      }

      currentTime = millis();
      decrementTime = currentTime - cycleStartTime;
      
      if (pumpFastSeconds[i] > 0) {
        pumpFastSeconds[i] = pumpFastSeconds[i] - decrementTime;
      }
      else if (pumpSlowSeconds[i] > 0) {
        pumpSlowSeconds[i] = pumpSlowSeconds[i] - decrementTime;
      }
//      else if (pumpLiveCycleTime[i] > 0 && isDeadTime == false){
//        pumpLiveCycleTime[i] = pumpLiveCycleTime[i] - decrementTime;
//        cycTimeAccum += decrementTime; //Accumulate the amount of time 
//      }
//      //Open question: do we really need to track dead time?
//      //Probably just get rid of pumpDeadCycleTime array and just use the dead accumulator
//      else if (pumpDeadCycleTime[i] > 0 && isDeadTime == true){
//        pumpDeadCycleTime[i] = pumpDeadCycleTime[i] - decrementTime;
//        cycTimeAccum += decrementTime;
//    }
      else if(pumpFastSeconds[i] <= 0 && pumpSlowSeconds[i] <= 0){// && pumpLiveCycleTime[i] <= 0){
        pumpOff(i+1);
        pumpsDone[i] = true;
        updateActivePumpLCD(i+1, pumpsDone[i]);
      }
    }

    //Check time and decrement from servo arrays
    //currentTime = millis();
    //decrementTime = currentTime - cycleStartTime;
    for (int i = 0; i < NUMBER_OF_SERVOS; i++) {

      if (servosDone[i] == true){
        continue;
      }

      currentTime = millis();
      decrementTime = currentTime - cycleStartTime;
      
      if (servoFastSeconds[i] > 0) {
        servoFastSeconds[i] = servoFastSeconds[i] - decrementTime;
      }
      else if (servoSlowSeconds[i] > 0) {
        servoSlowSeconds[i] = servoSlowSeconds[i] - decrementTime;
      }
      else {
        servoOff(i+1);
        servosDone[i] = true;
        updateActivePumpLCD(i+1+NUMBER_OF_PUMPS, servosDone[i]);
      }
    }

    //check if we are All Done
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
      if (pumpsDone[i] == false) {
        allPumpsDone = false;
        break;
      }
      else if (pumpsDone[i] == true) {
        allPumpsDone = true;
      }
    }
    for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
      if (servosDone[i] == false) {
        allServosDone = false;
        break;
      }
      else if (servosDone[i] == true) {
        allServosDone = true;
      }
    }
    
    if (allPumpsDone && allServosDone) {
      allDone = true;
      //Victory lap
      Serial.print("Elapsed Time: ");
      Serial.println(millis()-startTime);
      Serial.print("Total loops of dispenseLiquid Function: ");
      Serial.println(dLLoops);
      //lcd.print("dL Done");
      //lcd.setCursor(0,1);
      //lcd.print(dLLoops);
    }
  }
  
  return allDone;
}

//Allows for manual activation of individual pumps
//GOALS: Activate a given pump or servo for as long as a momentary pushbutton is held
  //Support for touchscreen interface?
    //PC sends token to enter jog mode
    //PC sends a token when the button is first pressed, pump activates, and checks for a stop token
    //Maybe make these two separate functions, one for the physical version, and another if there is a touch version
void jogMode(){
  //jogEnable should be a toggle switch, could have the LED in series, eliminating the need for a separate LED pin
  while(digitalRead(jogEnable)){
    for(int i = 0; i < sizeof(jogPumpPins); i++){
      if(digitalRead(jogPumpPins[i])){
        pumpOnFull(i+1);//pumpOnFull takes pumpID as arg, one more than loop index
        updateActivePumpLCD(i+1, false);
      }
      else{
        pumpOff(i+1);
        updateActivePumpLCD(i+1, true);
      }
    }

    for(int i = 0; i < sizeof(jogServoPins); i++) {
      if(digitalRead(jogServoPins[i])){
        servoOnFull(i+1+NUMBER_OF_PUMPS);
        updateActivePumpLCD(i+1+NUMBER_OF_PUMPS, false);
      }
      else{
        servoOff(i+1+NUMBER_OF_PUMPS);
        updateActivePumpLCD(i+1+NUMBER_OF_PUMPS, true);
      }
    }
  }
  //Make sure everything is off before exiting the function
  stopAllPumps();
  stopAllServos();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//Sensors and Data Management Functions
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


//Reset the global data arrays, for use after an order is completed
void zeroDataArrays(){
  for(int i = 0; i < NUMBER_OF_PUMPS; i++){
    pumpIDs[i] = 0;
    pumpFastSeconds[i] = 0;
    pumpSlowSeconds[i] = 0;
    pumpCycles[i] = 0;
  }
  for(int i = 0; i < NUMBER_OF_SERVOS; i++){
    servoIDs[i] = 0;
    servoFastSeconds[i] = 0;
    servoSlowSeconds[i] = 0;
  }
}

//Clear the serial buffer, for use after an order is completed
void clearSerialBuffer(){
  Serial.println("clearSerialBuffer: EOF########"); //Terminator to signal to the computer to close the COM port >= 8 "#'s"
  //Use println to send a newline after every message for the PC.
  while(Serial.available()){Serial.read();}
}



//Poll the DS18B20 temperature probe
//THIS FUNCTION takes about 516ms to complete(!)
float checkTemperature(){
  sensors.requestTemperatures();
  return sensors.getTempCByIndex(0);
}

//Updates the temperature displayed on the debug LCD at a fixed interval
void temperatureLCDUpdate(float currentTemp){
  static unsigned long initialTime = millis();
  static unsigned long timeElapsed = 0;
  //Update the temperature on the LCD every 1 seconds
  if (timeElapsed >= 1000){
    lcd.setCursor(6,0);
    lcd.print(currentTemp);
    initialTime = millis();
    timeElapsed = 0;
  }
  //If not enough time has passed to update the temperature on the LCD, increment timeElapsed
  timeElapsed = millis() - initialTime;
}

//Toggles the exhaust fan system at a fixed interval
void exhaustFanCheck(float currentTemp){
  static unsigned long fanStart = millis();
  static unsigned long fanElapsed = 0;
  //Check if you need to turn on the fan every 30 seconds
  if (fanElapsed >= 30000) {
    if (currentTemp < 45){
      digitalWrite(fanTransistorPin, LOW);
      lcd.setCursor(17,0);
      lcd.print("OFF");
    }
    else if (currentTemp > 55){
      digitalWrite(fanTransistorPin, HIGH);
      lcd.setCursor(17,0);
      lcd.print("ON ");
    }
    fanElapsed = 0;
    fanStart = millis();
  }
  //If the time does not exceed the fan check period, increment fanElapsed
  fanElapsed = millis() - fanStart;
}

//Prints the active pumps/servos to the debug LCD
void updateActivePumpLCD(int pumpID, boolean isDone){
  switch(pumpID){
    case 1:
      lcd.setCursor(0, 3);
    case 2:
      lcd.setCursor(2, 3);
    case 3:
      lcd.setCursor(4, 3);
    case 4:
      lcd.setCursor(6, 3);
    case 5:
      lcd.setCursor(8, 3);
    case 6:
      lcd.setCursor(10, 3);
    case 7:
      lcd.setCursor(12, 3);
    case 8:
      lcd.setCursor(14, 3);
    case 9:
      lcd.setCursor(16, 3);
    case 10:
      lcd.setCursor(18, 3);
  }

  if(isDone){
    if(pumpID < 10){
      lcd.print(" ");
    }
    else{
      lcd.print("  ");
    }
  }
  
  else{
    lcd.print(pumpID);
  }
  
}
/*************************************************/
/*Main Loop
 *
 */
/*************************************************/
void loop() {

  float currentTemp;
  lcd.setCursor(6,1);
  lcd.print("Rdy for Order");
  
  while(!Serial.available()) { 
    //This is the idle loop while waiting for serial data (to begin pumping)
    //Try to keep this short to minimize time between drink order and execution

    currentTemp = checkTemperature(); //get the current temperature
    temperatureLCDUpdate(currentTemp); //check if it's time to update the temp on the LCD
    exhaustFanCheck(currentTemp); //check if the exhaust fans need to be turned on/off
    //Start jogMode if enabled 
    if(digitalRead(jogEnable)){
      jogMode(); 
    }
  }

  //Checking if there is drink data ready to be sent to the Arduino
  if(Serial.peek() == HANDSHAKE_RX){
    Serial.println(HANDSHAKE_TX);  //send '!!!!!!!!' characters to the PC
    //Using println to add a newline, do so after sending any data to be interpreted by the PC
    lcd.setCursor(6,1);
    lcd.print("Sent Confirm!");
    delay(10);//wait some amount of time for the PC to respond with the serial data
    //this amount of time must be long enough for the message to begin, but not too long such that it is cut off
    
    //After serial data is recieved, try to dispenseLiquid
    if(receiveSerialData()){
      lcd.setCursor(6,1); //Step Update
      lcd.print("Pumping       "); //add padding to make a total of 14 characters to overwrite old step
      Serial.println("Serial data marked as recieved");
      if (dispenseLiquid()){
        Serial.println("dispenseLiquid: completed successfully");
      }
      else {
        lcd.setCursor(6,1); //Step Update
        lcd.print("Pump cancel");
        Serial.println("dispenseLiquid: terminated");
      }
    }
    else {
      lcd.setCursor(5,2);//Err update
      lcd.print("Serial Fail");
      errorcounter++;
      lcd.setCursor(18,2);
      lcd.print(errorcounter);
      
    }
    
    //After the pumping sequence
    //Clear the data and prepare for the next order 
    zeroDataArrays();
    clearSerialBuffer(); 
  }

  
}

//********************
//Deprecated functions
//********************


////THIS WONT WORK BECAUSE IT WILL BE STUCK IN THIS FUNCTION FOR THE DURATION INSTEAD OF LOOPING IN THE MAIN LOOP
//  //Change to just pump 1 mL and do the looping/waiting int the main loop 
//void pumpIntermittent(int pumpID, int cycles){
//  unsigned long startTime;
//  unsigned long currentTime;
//  unsigned long elapsedTime;
//  
//  if (pumpID <= 0 || pumpID > NUMBER_OF_PUMPS){
//    Serial.println("pumpIntermittent: pumpID out of range");
//  }
//  else if(pumpID > 0 && pumpID <= NUMBER_OF_PUMPS) {
//    for(int i = 0; i <= cycles; i++){
//      startTime = millis();
//      while(elapsedTime < SLOW_1ML_RATE){
//        pumpOnSlow(pumpID);
//        currentTime = millis();
//        elapsedTime = currentTime - startTime;
//      }
//      pumpOff(pumpID);
//      while(millis() - startTime < 500){ //wait for 500ms from the start of the last cycle before starting the next
//        ;
//      }
//    }
//  }
//  else {
//    Serial.println("pumpIntermittent: Invalid pumpID");
//  }
//}

////Prints the active pumps/servos to the debug LCD
//void updateActivePumpLCD(int pumpID, boolean isDone){
//  lcd.setCursor(pumpID-1, 3);
//  //if the pump is active
//  if(isDone){
//    if(pumpID >= 10){
//      lcd.print("  ");
//    }
//    else{
//      lcd.print(" "); 
//    } 
//  }
//  else {
//    lcd.print(pumpID);
//  }
//  
//}

/* Ideas
 *  
 * what if you coupled a rotary encoder to a peristaltic pump and used a PID controller to pump bidirectionally
more accurate pumping

does it take longer if its serial printing to an open serial port than if .end()

IR sensor on tube to determine if there is liquid
  from bottle, sensitve enough to detect an air gap?
IR sensor to measure peri. pump rotations
  laser and photoresistor near metal to detect rotations
Hall effect sensor to measure the metal spokes on the peristaltic pump
  count turns on external board, use interrupt once the count is high enough? 
  megnetic encoder disk + hall effect sensor

Mouse scroll wheel uses an encoder
 */
