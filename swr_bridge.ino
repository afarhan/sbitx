/* 
How to burn this into ATTINY85 
ATTINY85, 
Burning the bootloader:

1. use arduino 2.0.0 beta
2. Board is DIYTiny > ATTiny85
3. Use Bootloader > Yes, Normal Serial/USB Upload
4. Programmer : DIYTiny ATTiny: Arduino as ISP
5. Upload Bootloader

Burning in the Sketch
1. Sketch > Upload using Programmer

Adapted from Wire Slave Sender by Nicholas Zambetti <http://www.zambetti.com>

*/

#include <Wire.h>
int16_t fwd, ref;
byte message[4], flag;

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
  Wire.write(message, 4); // respond with message of 6 bytes
  // as expected by master
}

void setup() {
  Wire.begin(8);                // join i2c bus with address #8
  Wire.onRequest(requestEvent); // register event
  flag = 0;  
  pinMode(5, OUTPUT);
}

void loop() {
  delay(100);
  fwd = analogRead(A2);
  ref = analogRead(A3);
  message[0] = fwd & 0xff;
  message[1] = fwd >> 8;
  message[2] = ref & 0xff;
  message[3] = ref >> 8;
  if(flag == 1){
    digitalWrite(5, LOW);
    flag = 0;    
  }
  else {
    digitalWrite(5, HIGH);
    flag = 1;      
  }
}

