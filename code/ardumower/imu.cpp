/*
  Ardumower (www.ardumower.de)
  Copyright (c) 2013-2014 by Alexander Grau
  Copyright (c) 2013-2014 by Sven Gennat
  
  Private-use only! (you need to ask for a commercial-use)
 
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
  
  Private-use only! (you need to ask for a commercial-use)
*/

#include "imu.h"
#include <Arduino.h>
#include <Wire.h>
#include "drivers.h"
#include "i2c.h"
#include "config.h"
#include "flashmem.h"
#include "buzzer.h"

#if defined (IMU_GY801)
  // -------------I2C addresses ------------------------
  #define ADXL345B (0x53)          // ADXL345B acceleration sensor (GY-80 PCB)
  #define HMC5883L (0x1E)          // HMC5883L compass sensor (GY-80 PCB)
  #define MMC5883MA (0x30)         // MMC5883MA compass sensor (GY-801 PCB)
  #define L3G4200D (0xD2 >> 1)     // L3G4200D gyro sensor (GY-80 PCB)
#elif defined (IMU_MPU9250)
  MPU9250 MPU92xx(MPU9250_ADDRESS_AD0_HIGH);
  int status;
#endif

#define ADDR 600
#define MAGIC 6


struct {
  uint8_t xl;
  uint8_t xh;
  uint8_t yl;
  uint8_t yh;
  uint8_t zl;
  uint8_t zh;
} gyroFifo[32];



IMU::IMU(){
  hardwareInitialized = false;
  calibrationAvail = false;
  state = IMU_RUN;
  callCounter = 0;  
  errorCounter = 0;
  resetTryCount = 0;
  imuResetSuccessCounter = 0;
  
  gyroOfs.x=gyroOfs.y=gyroOfs.z=0;  
  gyroNoise = 0;      
  gyroCounter = 0; 
  useGyroCalibration = false;
  lastGyroTime = millis();
  
  accelCounter = 0;
  calibAccAxisCounter = 0;
  useAccCalibration = true; 
  accPitch = 0;
  accRoll = 0;
  ypr.yaw=ypr.pitch=ypr.roll = 0;
  
  accMin.x=accMin.y=accMin.z = 0;
  accMax.x=accMax.y=accMax.z = 0;  
  accOfs.x=accOfs.y=accOfs.z = 0;
  accScale.x=accScale.y=accScale.z = 2;  
  com.x=com.y=com.z=0;  
  
  comScale.x=comScale.y=comScale.z=2;  
  comOfs.x=comOfs.y=comOfs.z=0;    
  useComCalibration = true;
  type5588 = -1;
}

// rescale to -PI..+PI
float IMU::scalePI(float v)
{
  float d = v;
  while (d < 0) d+=2*PI;
  while (d >= 2*PI) d-=2*PI;
  if (d >= PI) return (-2*PI+d); 
  else if (d < -PI) return (2*PI+d);
  else return d;  
}

// rescale to -180..+180
float IMU::scale180(float v)
{
  float d = v;
  while (d < 0) d+=2*180;
  while (d >= 2*180) d-=2*180;
  if (d >= 180) return (-2*180+d); 
  else if (d < -180) return (2*180+d);
  else return d;  
}


// computes minimum distance between x radiant (current-value) and w radiant (set-value)
float IMU::distancePI(float x, float w)
{
  // cases:   
  // w=330 degree, x=350 degree => -20 degree
  // w=350 degree, x=10  degree => -20 degree
  // w=10  degree, x=350 degree =>  20 degree
  // w=0   degree, x=190 degree => 170 degree
  // w=190 degree, x=0   degree => -170 degree 
  float d = scalePI(w - x);
  if (d < -PI) d = d + 2*PI;
  else if (d > PI) d = d - 2*PI;  
  return d;
}

float IMU::distance180(float x, float w)
{
  float d = scale180(w - x);
  if (d < -180) d = d + 2*180;
  else if (d > 180) d = d - 2*180;  
  return d;
}


// weight fusion (w=0..1) of two radiant values (a,b)
float IMU::fusionPI(float w, float a, float b)
{ 
  float c;
  if ((b >= PI/2) && (a <= -PI/2)){
    c = w * a + (1.0-w) * (b-2*PI);
  } else if ((b <= -PI/2) && (a >= PI/2)){
    c = w * (a-2*PI) + (1.0-w) * b;
  } else c = w * a + (1.0-w) * b;
  return scalePI(c);
}

void IMU::loadSaveCalib(boolean readflag){
  int addr = ADDR;
  short magic = MAGIC;
  eereadwrite(readflag, addr, magic); // magic
  eereadwrite(readflag, addr, accOfs);
  eereadwrite(readflag, addr, accScale);    
  eereadwrite(readflag, addr, comOfs);
  eereadwrite(readflag, addr, comScale);      
}

void IMU::loadCalib(){
  short magic = 0;
  int addr = ADDR;
  eeread(addr, magic);
  if (magic != MAGIC) {
    Console.println(F("IMU error: no calib data"));
    return;  
  }
  calibrationAvail = true;
  Console.println(F("IMU: found calib data"));
  loadSaveCalib(true);
}

void IMU::saveCalib(){
  loadSaveCalib(false);
}

void IMU::deleteCalib(){
  int addr = ADDR;
  eewrite(addr, (short)0); // magic  
  accOfs.x=accOfs.y=accOfs.z=0;
  accScale.x=accScale.y=accScale.z=2;  
  comOfs.x=comOfs.y=comOfs.z=0;
  comScale.x=comScale.y=comScale.z=2;  
  Console.println(F("IMU calibration deleted"));  
}

void IMU::printPt(point_float_t p){
  Console.print(p.x);
  Console.print(F(","));    
  Console.print(p.y);  
  Console.print(F(","));
  Console.println(p.z);  
}

void IMU::printCalib(){
  Console.println(F("--------"));
  Console.print(F("accOfs="));
  printPt(accOfs);
  Console.print(F("accScale="));
  printPt(accScale);
  Console.print(F("comOfs="));
  printPt(comOfs);
  Console.print(F("comScale="));
  printPt(comScale);  
  Console.println(F("--------"));
}



// calculate gyro offsets
void IMU::calibGyro(){
  Console.println(F("---calibGyro---"));    
  useGyroCalibration = false;
  gyroOfs.x = gyroOfs.y = gyroOfs.z = 0;
  point_float_t ofs;
  while(true){    
    float zmin =  99999;
    float zmax = -99999;  
    gyroNoise = 0;
    ofs.x = ofs.y = ofs.z = 0;      
    for (int i=0; i < 50; i++){
      delay(10);
      readGyro();      
      zmin = min(zmin, gyro.z);
      zmax = max(zmax, gyro.z);
      ofs.x += ((float)gyro.x)/ 50.0;
      ofs.y += ((float)gyro.y)/ 50.0;
      ofs.z += ((float)gyro.z)/ 50.0;          
      gyroNoise += sq(gyro.z-gyroOfs.z) /50.0;   // noise is computed with last offset calculation
    }
    Console.print(F("gyro calib min="));
    Console.print(zmin);
    Console.print(F("\tmax="));    
    Console.print(zmax);
    Console.print(F("\tofs="));        
    Console.print(ofs.z);    
    Console.print(F("\tnoise="));                
    Console.println(gyroNoise);  
    if (gyroNoise < 20) break; // optimum found    
    gyroOfs = ofs; // new offset found
  }  
  useGyroCalibration = true;
  Console.print(F("counter="));
  Console.println(gyroCounter);  
  Console.print(F("ofs="));
  printPt(gyroOfs);  
  Console.println(F("------------"));  
}      

// Acceleration sensor driver
void  IMU::initAccel(){
  Console.println(F("initAccel"));
  #if defined (IMU_GY801)
    I2CwriteTo(ADXL345B, 0x2D, 0);
    I2CwriteTo(ADXL345B, 0x2D, 16);
    I2CwriteTo(ADXL345B, 0x2D, 8);         
  #elif defined (IMU_MPU9250)
    MPU92xx.beginAccel(ACC_FULL_SCALE_8_G);
  #endif
}

bool IMU::readAccel(){  
  #if defined (IMU_GY801)
    uint8_t buf[6];
    if (I2CreadFrom(ADXL345B, 0x32, 6, (uint8_t*)buf) != 6){
      Console.println(F("Failed to readADXL345B"));
      return false;
    }
  #elif defined (IMU_MPU9250)
    MPU92xx.accelUpdate();
  #endif
  // Convert the accelerometer value to G's. 
  // With 10 bits measuring over a +/-4g range we can find how to convert by using the equation:
  // Gs = Measurement Value * (G-range/(2^10)) or Gs = Measurement Value * (8/1024)
  // ( *0.0078 )
  #if defined (IMU_GY801)
    float x = (int16_t) (((uint16_t)buf[1]) << 8 | buf[0]); 
    float y = (int16_t) (((uint16_t)buf[3]) << 8 | buf[2]); 
    float z = (int16_t) (((uint16_t)buf[5]) << 8 | buf[4]);
  #elif defined (IMU_MPU9250)
    float x = MPU92xx.accelX();
    float y = MPU92xx.accelY();
    float z = -MPU92xx.accelZ();
  #endif
  //Console.println(z);
  if (useAccCalibration){
    x -= accOfs.x;
    y -= accOfs.y;
    z -= accOfs.z;
    x /= accScale.x*0.5;    
    y /= accScale.y*0.5;    
    z /= accScale.z*0.5;
    acc.x = x;
    //Console.println(z);
    acc.y = y;
    acc.z = z;
  } else {
    acc.x = x;
    acc.y = y;
    acc.z = z;
  }
  /*float accXreal = x + sin(gyroYpr.pitch);
  accXmax = max(accXmax, accXreal );    
  accXmin = min(accXmin, accXreal );        */
  //float amag = sqrt(Xa*Xa + Ya*Ya + Za*Za);
  //Xa /= amag;
  //Ya /= amag;
  //Za /= amag;  
  accelCounter++;
  return true;
}

// Gyro sensor driver
boolean IMU::initGyro(){
  Console.println(F("initGyro"));
  uint8_t buf[6];    
  int retry = 0;
  #if defined (IMU_GY801)
    while (true){
      I2CreadFrom(L3G4200D, 0x0F, 1, (uint8_t*)buf);
      if (buf[0] != 0xD3) {        
        Console.println(F("gyro read error"));
        retry++;
        if (retry > 2){
          errorCounter++;
          return false;
        }
        delay(1000);            
      } else break;
    }
    // Normal power mode, all axes enabled, 100 Hz
    I2CwriteTo(L3G4200D, 0x20, 0b00001100);    
    // 2000 dps (degree per second)
    I2CwriteTo(L3G4200D, 0x23, 0b00100000);      
    I2CreadFrom(L3G4200D, 0x23, 1, (uint8_t*)buf);
    if (buf[0] != 0b00100000){
        Console.println(F("gyro write error")); 
        while(true);
    }  
    // fifo mode 
   // I2CwriteTo(L3G4200D, 0x24, 0b01000000);        
   // I2CwriteTo(L3G4200D, 0x2e, 0b01000000);          
  #elif defined (IMU_MPU9250)
    // 2000 dps (degree per second)
    MPU92xx.beginGyro(GYRO_FULL_SCALE_2000_DPS);
  #endif
  delay(250);
  calibGyro();    
  return true;
}

boolean IMU::readGyro(){  
  #if defined (IMU_GY801)
    now = millis();
    uint8_t fifoSrcReg = 0;  

    I2CreadFrom(L3G4200D, 0x2F, sizeof(fifoSrcReg), &fifoSrcReg);         // read the FIFO_SRC_REG

    unsigned long duration = millis() - now;

    if (duration > 10) {
      // That's insane, shouldn't happen
      return false;
    }

     // FIFO_SRC_REG
     // 7: Watermark status. (0: FIFO filling is lower than WTM level; 1: FIFO filling is equal or higher than WTM level)
     // 6: Overrun bit status. (0: FIFO is not completely filled; 1:FIFO is completely filled)
     // 5: FIFO empty bit. (0: FIFO not empty; 1: FIFO empty)
     // 4..0: FIFO stored data level
     //Console.print("FIFO_SRC_REG: "); Console.println(fifoSrcReg, HEX);
    uint8_t countOfData = (fifoSrcReg & 0x1F) + 1;   
    //  if (bitRead(fifoSrcReg, 6)==1) Console.println(F("IMU error: FIFO overrun"));

    memset(gyroFifo, 0, sizeof(gyroFifo[0])*32);
    I2CreadFrom(L3G4200D, 0xA8, sizeof(gyroFifo[0])*countOfData, (uint8_t *)gyroFifo);         // the first bit of the register address specifies we want automatic address increment
    //I2CreadFrom(L3G4200D, 0x28, sizeof(gyroFifo[0])*countOfData, (uint8_t *)gyroFifo);         // the first bit of the register address specifies we want automatic address increment
  #elif defined (IMU_MPU9250)
    MPU92xx.gyroUpdate();
    uint8_t countOfData = 1;
  #endif
  gyro.x = gyro.y = gyro.z = 0;
  //Console.print("fifo:");
  //Console.println(countOfData);
  if (!useGyroCalibration) countOfData = 1;
  for (uint8_t i=0; i<countOfData; i++){
      #if defined (IMU_GY801)
        gyro.x += (int16_t) (((uint16_t)gyroFifo[i].xh) << 8 | gyroFifo[i].xl);
        gyro.y += (int16_t) (((uint16_t)gyroFifo[i].yh) << 8 | gyroFifo[i].yl);
        gyro.z += (int16_t) (((uint16_t)gyroFifo[i].zh) << 8 | gyroFifo[i].zl);
      #elif defined (IMU_MPU9250)
        gyro.x += MPU92xx.gyroX();
        gyro.y += MPU92xx.gyroY();
        gyro.z += -MPU92xx.gyroZ();
      #endif
      if (useGyroCalibration){
        gyro.x -= gyroOfs.x;
        gyro.y -= gyroOfs.y;
        gyro.z -= gyroOfs.z;               
      }
  }
  if (useGyroCalibration){
    gyro.x *= 0.07 * PI/180;  // convert to radiant per second
    gyro.y *= 0.07 * PI/180; 
    gyro.z *= 0.07 * PI/180;      
  }
  gyroCounter++;
  return true;
}


// Compass sensor driver
void  IMU::initCom(){
  Console.println(F("initCom"));
  #if defined (IMU_GY801)
    // test the compass sensor HMC588L or MMC5883MA
    Wire.beginTransmission(HMC5883L);
    int error = Wire.endTransmission();

    if (!error && type5588 == -1) {
       type5588 = HMC5883L;
       Console.println(F("Found HMC5883L"));
    }
    
    Wire.beginTransmission(MMC5883MA);
    error = Wire.endTransmission();

    if (!error && type5588 == -1) {
       type5588 = MMC5883MA;
       Console.println(F("Found MMC5883MA"));
    }
    
    if (type5588 == HMC5883L) {
       I2CwriteTo(HMC5883L, 0x00, 0x70);  // 8 samples averaged, 75Hz frequency, no artificial bias.       
       //I2CwriteTo(HMC5883L, 0x01, 0xA0);      // gain
       I2CwriteTo(HMC5883L, 0x01, 0x20);   // gain
       I2CwriteTo(HMC5883L, 0x02, 00);    // mode         
    }
    else if (type5588 == MMC5883MA) {
       uint8_t buf[6];
       I2CwriteTo(MMC5883MA, 0x08, 0x0);  // 
       I2CwriteTo(MMC5883MA, 0x09, 0x00);   // 16bit / 1.6ms / no inbihit
       I2CwriteTo(MMC5883MA, 0x0A, 0x01);  // 14/28/54/84Hz frequency         
       // Calibrate sensor
       // Set SET
       I2CwriteTo(MMC5883MA,0x8,0x08);
       I2CreadFrom(MMC5883MA, 0x00, 6, (uint8_t*)buf);
       float x = (int16_t) ((((uint16_t)buf[1]) << 8) | buf[0]);
       float y = (int16_t) ((((uint16_t)buf[3]) << 8) | buf[2]);
       float z = (int16_t) ((((uint16_t)buf[5]) << 8) | buf[4]);  
       // Set RESET
       I2CwriteTo(MMC5883MA,0x8,0x10);
       I2CreadFrom(MMC5883MA, 0x00, 6, (uint8_t*)buf);
       x -= (int16_t) ((((uint16_t)buf[1]) << 8) | buf[0]);
       y -= (int16_t) ((((uint16_t)buf[3]) << 8) | buf[2]);
       z -= (int16_t) ((((uint16_t)buf[5]) << 8) | buf[4]);  
       x /= 2; y /= 2; z /= 2;
       // Clr RESET
       I2CwriteTo(MMC5883MA,0x8,0x0);
       // Now x,y,z are the correction factor, save them
       MMCOffset[0] = x;
       MMCOffset[1] = y;
       MMCOffset[2] = z;
       if (I2CreadFrom(MMC5883MA, 0x00, 6, (uint8_t*)buf) != 6){
          errorCounter++;
          return;
       }
    }
  #elif defined (IMU_MPU9250)
    MPU92xx.beginMag(MAG_MODE_CONTINUOUS_100HZ);
  #endif
}

bool IMU::readCom(){    
  uint8_t buf[6];  
  float x;
  float y;
  float z;

  #if defined (IMU_GY801)
    if (type5588 == HMC5883L) {
       if (I2CreadFrom(HMC5883L, 0x03, 6, (uint8_t*)buf) != 6){
         Console.println(F("Failed to readHMC5883L"));
         return false;
       }
       // scale +1.3Gauss..-1.3Gauss  (*0.00092)  
       x = (int16_t) ((((uint16_t)buf[0]) << 8) | buf[1]);
       y = (int16_t) ((((uint16_t)buf[4]) << 8) | buf[5]);
       z = (int16_t) ((((uint16_t)buf[2]) << 8) | buf[3]);  
    }
    else if (type5588 == MMC5883MA) {
       if (I2CreadFrom(MMC5883MA, 0x00, 6, (uint8_t*)buf) != 6){
         Console.println(F("Failed to readMMC5883MA"));
         return false;
       }
       x = (int16_t) ((((uint16_t)buf[1]) << 8) | buf[0]);
       y = (int16_t) ((((uint16_t)buf[3]) << 8) | buf[2]);
       z = (int16_t) ((((uint16_t)buf[5]) << 8) | buf[4]);  
       x -= MMCOffset[0];
       y -= MMCOffset[1];
       z -= MMCOffset[2];
    }
  #elif defined (IMU_MPU9250)
    MPU92xx.magUpdate();
    // scale +1.3Gauss..-1.3Gauss  (*0.00092)  
    x = (int16_t) MPU92xx.magX();
    y = (int16_t) MPU92xx.magY();
    z = (int16_t) MPU92xx.magZ();
  #endif
  if (useComCalibration){
    x -= comOfs.x;
    y -= comOfs.y;
    z -= comOfs.z;
    x /= comScale.x*0.5;    
    y /= comScale.y*0.5;    
    z /= comScale.z*0.5;
    com.x = x;
    //Console.println(z);
    com.y = y;
    com.z = z;
  } else {
    com.x = x;
    com.y = y;
    com.z = z;
  }  
  return true;
}

float IMU::sermin(float oldvalue, float newvalue){
  if (newvalue < oldvalue) {
    Console.print(F("."));
    digitalWrite(pinLED, true);
  }
  return min(oldvalue, newvalue);
}

float IMU::sermax(float oldvalue, float newvalue){
  if (newvalue > oldvalue) {
    Console.print(F("."));
    digitalWrite(pinLED, true);
  }
  return max(oldvalue, newvalue);
}

void IMU::calibComStartStop(){  
  while (Console.available()) Console.read();  
  if (state == IMU_CAL_COM){
    // stop 
    Console.println(F("com calib completed"));    
    calibrationAvail = true;
    float xrange = comMax.x - comMin.x;
    float yrange = comMax.y - comMin.y;
    float zrange = comMax.z - comMin.z;
    comOfs.x = xrange/2 + comMin.x;
    comOfs.y = yrange/2 + comMin.y;
    comOfs.z = zrange/2 + comMin.z;
    comScale.x = xrange;
    comScale.y = yrange;  
    comScale.z = zrange;
    saveCalib();  
    printCalib();
    useComCalibration = true; 
    state = IMU_RUN;    
    // completed sound
    Buzzer.tone(600);
    delay(200); 
    Buzzer.tone(880);
    delay(200); 
    Buzzer.tone(1320);              
    delay(200); 
    Buzzer.noTone();    
    delay(500);
  } else {
    // start
    Console.println(F("com calib..."));
    Console.println(F("rotate sensor 360 degree around all three axis"));
    foundNewMinMax = false;  
    useComCalibration = false;
    state = IMU_CAL_COM;  
    comMin.x = comMin.y = comMin.z = 9999;
    comMax.x = comMax.y = comMax.z = -9999;
  }
}

boolean IMU::newMinMaxFound(){
  boolean res = foundNewMinMax;
  foundNewMinMax = false;
  return res;
}  
  
void IMU::calibComUpdate(){
  comLast = com;
  delay(20);
  readCom();  
  boolean newfound = false;
  if ( (abs(com.x-comLast.x)<10) &&  (abs(com.y-comLast.y)<10) &&  (abs(com.z-comLast.z)<10) ){
    if (com.x < comMin.x) { 
      comMin.x = com.x;
      newfound = true;      
    }
    if (com.y < comMin.y) { 
      comMin.y = com.y;
      newfound = true;      
    }
    if (com.z < comMin.z) { 
      comMin.z = com.z;
      newfound = true;      
    }
    if (com.x > comMax.x) { 
      comMax.x = com.x;
      newfound = true;      
    }
    if (com.y > comMax.y) { 
      comMax.y = com.y;
      newfound = true;      
    }
    if (com.z > comMax.z) { 
      comMax.z = com.z;
      newfound = true;      
    }    
    if (newfound) {      
      foundNewMinMax = true;
      Buzzer.tone(440);
      Console.print(F("x:"));
      Console.print(comMin.x);
      Console.print(F(","));
      Console.print(comMax.x);
      Console.print(F("\t  y:"));
      Console.print(comMin.y);
      Console.print(F(","));
      Console.print(comMax.y);
      Console.print(F("\t  z:"));
      Console.print(comMin.z);
      Console.print(F(","));
      Console.print(comMax.z);    
      Console.println(F("\t"));
    } else Buzzer.noTone();   
  }    
}

// calculate acceleration sensor offsets
boolean IMU::calibAccNextAxis(){  
  boolean complete = false;
  Buzzer.tone(440);
  while (Console.available()) Console.read();  
  useAccCalibration = false;  
  if (calibAccAxisCounter >= 6) calibAccAxisCounter = 0;
  if (calibAccAxisCounter == 0){
    // restart
    Console.println(F("acc calib restart..."));
    accMin.x = accMin.y = accMin.z = 99999;
    accMax.x = accMax.y = accMax.z = -99999;    
  }
  point_float_t pt = {0,0,0};
  for (int i=0; i < 100; i++){        
    readAccel();            
    pt.x += acc.x / 100.0;
    pt.y += acc.y / 100.0;
    pt.z += acc.z / 100.0;                  
    Console.print(acc.x);
    Console.print(F(","));
    Console.print(acc.y);
    Console.print(F(","));
    Console.println(acc.z);
    delay(1);
  }
  accMin.x = min(accMin.x, pt.x);
  accMax.x = max(accMax.x, pt.x);         
  accMin.y = min(accMin.y, pt.y);
  accMax.y = max(accMax.y, pt.y);         
  accMin.z = min(accMin.z, pt.z);
  accMax.z = max(accMax.z, pt.z);           
  calibAccAxisCounter++;        
  useAccCalibration = true;  
  Console.print(F("side "));
  Console.print(calibAccAxisCounter);
  Console.println(F(" of 6 completed"));    
  if (calibAccAxisCounter == 6){    
    // all axis complete 
    float xrange = accMax.x - accMin.x;
    float yrange = accMax.y - accMin.y;
    float zrange = accMax.z - accMin.z;
    accOfs.x = xrange/2 + accMin.x;
    accOfs.y = yrange/2 + accMin.y;
    accOfs.z = zrange/2 + accMin.z;
    accScale.x = xrange;
    accScale.y = yrange;  
    accScale.z = zrange;    
    printCalib();
    saveCalib();    
    Console.println(F("acc calibration completed"));    
    complete = true;
    // completed sound
    Buzzer.tone(600);
    delay(200); 
    Buzzer.tone(880);
    delay(200); 
    Buzzer.tone(1320);              
    delay(200); 
  };
  Buzzer.noTone();
  delay(500);
  return complete;
}      

// first-order complementary filter
// newAngle = angle measured with atan2 using the accelerometer
// newRate = angle measured using the gyro
// looptime = loop time in millis()
float Complementary2(float newAngle, float newRate,int looptime, float angle) {
  float k=10;
  float dtc2=float(looptime)/1000.0;
  float x1 = (newAngle -   angle)*k*k;
  float y1 = dtc2*x1 + y1;
  float x2 = y1 + (newAngle -   angle)*2*k + newRate;
  angle = dtc2*x2 + angle;
  return angle;
}

// second-order complementary filter
// a=tau / (tau + loop time)
// newAngle = angle measured with atan2 using the accelerometer
// newRate = angle measured using the gyro
// looptime = loop time in millis()
float Complementary(float newAngle, float newRate,int looptime, float angle) {
  float tau=0.075;
  float a=0.0;
  float dtC = float(looptime)/1000.0;
  a=tau/(tau+dtC);
  angle= a* (angle + newRate * dtC) + (1-a) * (newAngle);
  return angle;
}

// Kalman filter                                      
// newAngle = angle measured with atan2 using the accelerometer
// newRate = angle measured using the gyro
// looptime = loop time in millis()
float Kalman(float newAngle, float newRate,int looptime, float x_angle)
{
  float Q_angle  =  0.01; //0.001
  float Q_gyro   =  0.0003;  //0.003
  float R_angle  =  0.01;  //0.03

  float x_bias = 0;
  float P_00 = 0, P_01 = 0, P_10 = 0, P_11 = 0;
  float  y, S;
  float K_0, K_1;

  float dt = float(looptime)/1000;
  x_angle += dt * (newRate - x_bias);
  P_00 +=  - dt * (P_10 + P_01) + Q_angle * dt;
  P_01 +=  - dt * P_11;
  P_10 +=  - dt * P_11;
  P_11 +=  + Q_gyro * dt;

  y = newAngle - x_angle;
  S = P_00 + R_angle;
  K_0 = P_00 / S;
  K_1 = P_10 / S;

  x_angle +=  K_0 * y;
  x_bias  +=  K_1 * y;
  P_00 -= K_0 * P_00;
  P_01 -= K_0 * P_01;
  P_10 -= K_1 * P_00;
  P_11 -= K_1 * P_01;

  return x_angle;
}

// scale setangle, so that both PI angles have the same sign    
float scalePIangles(float setAngle, float currAngle){
  if ((setAngle >= PI/2) && (currAngle <= -PI/2)) return (setAngle-2*PI);
    else if ((setAngle <= -PI/2) && (currAngle >= PI/2)) return (setAngle+2*PI);
    else return setAngle;
}

void IMU::update(){
  if (!read())
    return;  
  now = millis();
  int looptime = (now - lastAHRSTime);
  lastAHRSTime = now;
  
  if (state == IMU_RUN){
    // ------ roll, pitch --------------  
    float forceMagnitudeApprox = abs(acc.x) + abs(acc.y) + abs(acc.z);    
    //if (forceMagnitudeApprox < 1.2) {
      //Console.println(forceMagnitudeApprox);      
      accPitch   = atan2(-acc.x , sqrt(sq(acc.y) + sq(acc.z)));         
      accRoll    = atan2(acc.y , acc.z);       
      accPitch = scalePIangles(accPitch, ypr.pitch);
      accRoll  = scalePIangles(accRoll, ypr.roll);
      // complementary filter            
      ypr.pitch = Kalman(accPitch, gyro.x, looptime, ypr.pitch);  
      ypr.roll  = Kalman(accRoll,  gyro.y, looptime, ypr.roll);            
    /*} else {
      //Console.print("too much acceleration ");
      //Console.println(forceMagnitudeApprox);
      ypr.pitch = ypr.pitch + gyro.y * ((float)(looptime))/1000.0;
      ypr.roll  = ypr.roll  + gyro.x * ((float)(looptime))/1000.0;
    }*/
    ypr.pitch=scalePI(ypr.pitch);
    ypr.roll=scalePI(ypr.roll);
    // ------ yaw --------------
    // tilt-compensated yaw
    comTilt.x =  com.x  * cos(ypr.pitch) + com.z * sin(ypr.pitch);
    comTilt.y =  com.x  * sin(ypr.roll)         * sin(ypr.pitch) + com.y * cos(ypr.roll) - com.z * sin(ypr.roll) * cos(ypr.pitch);
    comTilt.z = -com.x  * cos(ypr.roll)         * sin(ypr.pitch) + com.y * sin(ypr.roll) + com.z * cos(ypr.roll) * cos(ypr.pitch);     
    comYaw = scalePI( atan2(comTilt.y, comTilt.x)  );  
    comYaw = scalePIangles(comYaw, ypr.yaw);
    //comYaw = atan2(com.y, com.x);  // assume pitch, roll are 0
    // complementary filter
    ypr.yaw = Complementary2(comYaw, -gyro.z, looptime, ypr.yaw);
    ypr.yaw = scalePI(ypr.yaw);
  } 
  else if (state == IMU_CAL_COM) {
    calibComUpdate();
  }
}  

boolean IMU::init(){    
  loadCalib();
  printCalib();
  if (!hwInit()) return false;
  now = 0;  
  hardwareInitialized = true;
  return true;
}

bool IMU::hwInit(){
  #if defined (IMU_MPU9250)
    Wire.begin();
    MPU92xx.setWire(&Wire);
  #endif
    initAccel();
    initGyro();
    initCom();
  return true;
}

int IMU::getCallCounter(){
  int res = callCounter;
  callCounter = 0;
  return res;
}

int IMU::getErrorCounter(){
  int res = errorCounter;
  errorCounter = 0;
  return res;
}

bool IMU::read(){  
  if (!hardwareInitialized) {
    errorCounter++;
    return false;
  }
  callCounter++;    
  if (readAccel()
      && readGyro()
      && readCom()) {
    if (resetTryCount > 0) {
      // reset was successful
      imuResetSuccessCounter++;
      resetTryCount = 0;
    }
    errorCounter = 0;
    return true;
  } else {
    if (resetTryCount < 3) {
      // Try reseting without erroring out
      I2Creset();
      Wire.begin();
      resetTryCount++;
      return false;
    }
    errorCounter++;
    return false;
  }
}


