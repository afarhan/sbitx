/*
 * i2cBitBangingBus.cpp
 *
 *  Created on: 06.03.2015
 *      Author: "Marek Wyborski"
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <linux/types.h>
#include <stdint.h>
#include <time.h>
#include "i2cbb.h"

static uint8_t PIN_SDA;
static uint8_t PIN_SCL;
static uint32_t sleepTimeNanos;
static struct timespec nanoSleepTime;
static uint32_t delayTicks;
int i2c_started = 0;

void i2cbb_init(uint8_t pin_number_sda, uint8_t pin_number_scl) 
{
	PIN_SDA = pin_number_sda;
	PIN_SCL = pin_number_scl;
	sleepTimeNanos = 0;
	nanoSleepTime.tv_sec = 0;
	nanoSleepTime.tv_nsec = 0;	
	delayTicks = 0;
	i2c_started = 0;
  // Pull up setzen 50KΩ
  // http://wiringpi.com/reference/core-functions/
  //    pullUpDnControl(PIN_SDA,PUD_OFF);
  //    pullUpDnControl(PIN_SCL,PUD_OFF);

    nanoSleepTime.tv_sec = 0;
    nanoSleepTime.tv_nsec = 1;

}

// I2C implementation is copied and pasted from wikipedia:
// 
// https://en.wikipedia.org/wiki/I%C2%B2C#Example_of_bit-banging_the_I.C2.B2C_master_protocol
//
//

static int read_SCL(){ // Set SCL as input and return current level of line, 0 or 1
    pinMode(PIN_SCL, INPUT);
    return digitalRead(PIN_SCL);
}

static int read_SDA(){ // Set SDA as input and return current level of line, 0 or 1
    pinMode(PIN_SDA, INPUT);
    return digitalRead(PIN_SDA);
}

static void clear_SCL(){ // Actively drive SCL signal low
    pinMode(PIN_SCL, OUTPUT);
    digitalWrite(PIN_SCL, 0);
}

static void clear_SDA(){ // Actively drive SDA signal low
    pinMode(PIN_SDA, OUTPUT);
    digitalWrite(PIN_SDA, 0);
}

static void arbitration_lost(char * where) {
    printf("I2CBB connection lost:");
    puts(where);
}

static void i2c_sleep() {

    if (sleepTimeNanos)
#ifdef NO_NANOSLEEP
        usleep(sleepTimeNanos / 1000);
#else
        nanosleep(&nanoSleepTime, NULL);
#endif
}

static void i2c_delay() {
    unsigned int index;
    for (index = 0; index < delayTicks; index++)
        ;
}

static void i2c_start_cond() {
    if (i2c_started) { // if started, do a restart cond
      // set SDA to 1
        read_SDA();
        i2c_delay();
        while (read_SCL() == 0) {  // Clock stretching
            i2c_sleep();
        }
        // Repeated start setup time, minimum 4.7us
        i2c_delay();
    }
    if (read_SDA() == 0) {
        arbitration_lost("i2c_start_cond");
    }
    // SCL is high, set SDA from 1 to 0.
    clear_SDA();
    i2c_delay();
    clear_SCL();
    i2c_started = 1;
}

static void i2c_stop_cond(void) {
    // set SDA to 0
    clear_SDA();
    i2c_delay();
    // Clock stretching
    while (read_SCL() == 0) {
        // add timeout to this loop.
        i2c_sleep();
    }
    // Stop bit setup time, minimum 4us
    i2c_delay();
//  usleep(4);
    read_SDA();
    // SCL is high, set SDA from 0 to 1
    if (read_SDA() == 0) {
        arbitration_lost("i2c_stop_cond");
    }
    i2c_delay();
    i2c_started = 0;
}

// Write a bit to I2C bus
static void i2c_write_bit(int bit)
{
    if (bit) {
        read_SDA();
    }
    else {
        clear_SDA();
    }
    i2c_delay();
    while (read_SCL() == 0) { // Clock stretching
      // You should add timeout to this loop
        i2c_sleep();
    }
    // SCL is high, now data is valid
    // If SDA is high, check that nobody else is driving SDA
    if (bit && read_SDA() == 0) {
        arbitration_lost("i2c_write_bit");
    }
    i2c_delay();
    clear_SCL();
}

// Read a bit from I2C bus
static int i2c_read_bit() {
    int bit;
    // Let the slave drive data
    read_SDA();
    i2c_delay();
    while (read_SCL() == 0) { // Clock stretching
      // You should add timeout to this loop
        i2c_sleep();
    }
    // SCL is high, now data is valid
    bit = read_SDA();
    i2c_delay();
    clear_SCL();

//  cout << "Bit: " << (bit ? "1" : "0" )<< endl;

    return bit;
}

// Write a byte to I2C bus. Return 0 if ack by the slave.
static int i2c_write_byte(int send_start, int send_stop, uint8_t byte) {
    unsigned bit;
    int nack;
    if (send_start) {
        i2c_start_cond();
    }
    for (bit = 0; bit < 8; bit++) {
        i2c_write_bit((byte & 0x80) != 0);
        byte <<= 1;
    }
    nack = i2c_read_bit();
    if (send_stop) {
        i2c_stop_cond();
    }
    return nack;
}

// Read a byte from I2C bus
static uint8_t i2c_read_byte(int nack, int send_stop) {
    unsigned char byte = 0;
    unsigned bit;
    for (bit = 0; bit < 8; bit++) {
        byte = (byte << 1) | i2c_read_bit();
    }
    i2c_write_bit(nack);
    if (send_stop) {
        i2c_stop_cond();
    }
    return byte;
}

// KERNEL-LIKE I2C METHODS

// This executes the SMBus “write byte” protocol, returning negative errno else zero on success.
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            if (!i2c_write_byte(0, 1, value)) {
                return 0;
            }
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

// This executes the SMBus “read byte” protocol, returning negative errno else a data byte received from the device.
int32_t i2cbb_read_byte_data(uint8_t i2c_address, uint8_t command) {

    uint8_t address = (i2c_address << 1) | 0;
    if (!i2c_write_byte(1, 0, address)) {

        if (!i2c_write_byte(0, 0, command)) {

            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(1, 0, address)) {
                return i2c_read_byte(1, 1);
            }
            else
                i2c_stop_cond();
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

// This executes the SMBus “block write” protocol, returning negative errno else zero on success.
int32_t i2cbb_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values) {
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            int errors = 0;
            for (size_t i = 0; i < length; i++) {
                if (!errors) {
                    errors = i2c_write_byte(0, 0, values[i]);
                }
            }

            i2c_stop_cond();

            if (!errors)
                return 0;
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

// This executes the SMBus “block read” protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2cbb_read_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        uint8_t* values) {
    uint8_t address = (i2c_address << 1) | 0;
    if (!i2c_write_byte(1, 0, address)) {

        if (!i2c_write_byte(0, 0, command)) {

            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(1, 0, address)) {
                for (uint8_t i = 0; i < length; i++) {
                    values[i] = i2c_read_byte(i == (length - 1), i == (length - 1));
                }

                return length;
            }
            else
                i2c_stop_cond();
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

