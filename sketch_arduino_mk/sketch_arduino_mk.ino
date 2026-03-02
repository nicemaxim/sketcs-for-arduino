/*Михаил Юрьевич Барабанов the best
*/

#include <GyverFilters.h>
#include <GyverTM1637.h>
#include <Servo.h>
#include <iarduino_DHT.h>

#define CLK 11
#define DIO 12

Servo n;
Servo m;
iarduino_DHT sensor1(4);
iarduino_DHT sensor2(13);
GyverTM1637 disp(CLK, DIO);
uint32_t Now, clocktimer;
boolean flag;
unsigned long last;
int b, x, y;
int hrs = 2, mins = 0, h = 0, lll = 30;
uint32_t tmr;
unsigned long now;
boolean ffg;
boolean ggf;
GKalman testFilter(40, 40, 0.1);
GKalman testFilter2(40, 40, 0.1);


void setup() {
  disp.clear();
   disp.brightness(7); 
  n.attach(10);
  m.attach(6);
  Serial.begin(9600);
  pinMode(8, OUTPUT);
  pinMode(2, OUTPUT);
  pinMode(7, OUTPUT);

   n.write(0);
  m.write(0);
}


void loop() {

  ffg = digitalRead(2);

  normClock();

  if (millis() - last > 7200000) {
    last = millis();
    digitalWrite(9, HIGH);
    digitalWrite(2, HIGH);
  }

  if (ffg) {
    if (millis() - last > 1800000) {
      last = millis();
      digitalWrite(9, LOW);
      digitalWrite(2, LOW);
    }
  }

  switch (sensor1.read()) {
    case DHT_OK:
      Serial.println((String) "CEHCOP 1: " + sensor1.hum + "% - " + sensor1.tem + "*C");
      x = sensor1.hum;
      y = sensor1.tem;
      y= testFilter2.filtered((int)y);
      break;
    case DHT_ERROR_CHECKSUM: Serial.println("CEHCOP 1: HE PABEHCTBO KC"); break;
    case DHT_ERROR_DATA: Serial.println("CEHCOP 1: OTBET HE COOTBETCTB. CEHCOPAM 'DHT'"); break;
    case DHT_ERROR_NO_REPLY: Serial.println("CEHCOP 1: HET OTBETA"); break;
    default: Serial.println("CEHCOP 1: ERROR"); break;
  }
  switch (sensor2.read()) {
    case DHT_OK:
      Serial.println((String) "CEHCOP 2: " + sensor2.hum + "% - " + sensor2.tem + "*C");
      b = sensor2.tem;
      b = testFilter.filtered((int)b);
      break;
    case DHT_ERROR_CHECKSUM: Serial.println("CEHCOP 2: HE PABEHCTBO KC"); break;
    case DHT_ERROR_DATA: Serial.println("CEHCOP 2: OTBET HE COOTBETCTB. CEHCOPAM 'DHT'"); break;
    case DHT_ERROR_NO_REPLY: Serial.println("CEHCOP 2: HET OTBETA"); break;
    default: Serial.println("CEHCOP 2: ERROR"); break;
  }
  
  if (y >= 31) {
    n.write(80);
    m.write(80);
    digitalWrite(9, HIGH);
    if (b <= 28) {
      digitalWrite(9, LOW);
      n.write(0);
      m.write(0);
    }
  } else {
    digitalWrite(9, LOW);
    n.write(0);
    m.write(0);
    if (b <= 28) {
      digitalWrite(7, HIGH);
      digitalWrite(9, HIGH);
      if (y >= 31) {
        digitalWrite(9, LOW);
        digitalWrite(7, LOW);
      }
    } else
      digitalWrite(9, LOW);
    digitalWrite(7, LOW);
    if (x >= 80) {
      digitalWrite(7, HIGH);
      digitalWrite(9, HIGH);
      n.write(80);
      m.write(80);
      if (y >= 32 || x <= 60) {
        digitalWrite(7, LOW);
        digitalWrite(9, LOW);
        n.write(0);
        m.write(0);
      }
    }
  }
}

void normClock() {
  if (millis() - tmr >= 60000) {
    tmr = millis();
    flag = !flag;
    disp.point(flag);
    if (millis() - now >= 60000) {
      mins--;
      if (mins < 0) {
        mins = 59;
        hrs--;
        if (hrs < 0) {hrs = 1;
        ggf = 1;
      }
    }
    disp.displayClock(hrs, mins);
  }
  }
  if(ggf){
      if (millis() - tmr >= 60000) {
    tmr = millis();
    flag = !flag;
    disp.point(flag);
    if (millis() - now >= 60000) {
      lll--;
      if (lll < 0) {
        ggf = 0;
        lll = 30;}
    }
    disp.displayClock(h, lll);
  }
  }
    }
