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
  Wire.write(message, 4); // 4 bytes message with fwd and ref
}

void setup() {
  Wire.begin(8);                // join i2c bus with address #8
  Wire.onRequest(requestEvent); // register event
}

void loop() {
  delay(2);
  fwd = analogRead(A2);
  ref = analogRead(A3);
  message[0] = fwd & 0xff;
  message[1] = fwd >> 8;
  message[2] = ref & 0xff;
  message[3] = ref >> 8;
}
