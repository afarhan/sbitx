/*
 * i2cBitBangingBus.cpp
 *
 *  Created on: 06.03.2015
 *      Author: "Marek Wyborski"
 */

#include "i2cBitBangingBus.h"

#include <wiringPi.h>

#include <iostream>
#include <memory>
#include <stdexcept>

i2cBitBangingBus::i2cBitBangingBus(uint8_t pin_number_sda, uint8_t pin_number_scl, uint32_t sleepTimeNanos_,
        uint32_t delayTicks_) :
        PIN_SDA(pin_number_sda), PIN_SCL(pin_number_scl), sleepTimeNanos(sleepTimeNanos_), nanoSleepTime(), delayTicks(
                delayTicks_), i2c_started(false)
{
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



bool i2cBitBangingBus::read_SCL() // Set SCL as input and return current level of line, 0 or 1
{
    pinMode(PIN_SCL, INPUT);
    return digitalRead(PIN_SCL);
}

bool i2cBitBangingBus::read_SDA() // Set SDA as input and return current level of line, 0 or 1
{
    pinMode(PIN_SDA, INPUT);
    return digitalRead(PIN_SDA);
}

void i2cBitBangingBus::clear_SCL() // Actively drive SCL signal low
{
    pinMode(PIN_SCL, OUTPUT);
    digitalWrite(PIN_SCL, 0);
}

void i2cBitBangingBus::clear_SDA() // Actively drive SDA signal low
{
    pinMode(PIN_SDA, OUTPUT);
    digitalWrite(PIN_SDA, 0);
}

void i2cBitBangingBus::arbitration_lost(string where)
{
    throw runtime_error("Connection lost: " + where);
}

void i2cBitBangingBus::i2c_sleep()
{

    if (sleepTimeNanos)
#ifdef NO_NANOSLEEP
        usleep(sleepTimeNanos / 1000);
#else
        nanosleep(&nanoSleepTime, NULL);
#endif
}

void i2cBitBangingBus::i2c_delay()
{
    unsigned int index;
    for (index = 0; index < delayTicks; index++)
        ;
}

void i2cBitBangingBus::i2c_start_cond()
{
    if (i2c_started)
    { // if started, do a restart cond
      // set SDA to 1
        read_SDA();
        i2c_delay();
        while (read_SCL() == 0)
        {  // Clock stretching
            i2c_sleep();
        }
        // Repeated start setup time, minimum 4.7us
        i2c_delay();
    }
    if (read_SDA() == 0)
    {
        arbitration_lost("i2c_start_cond");
    }
    // SCL is high, set SDA from 1 to 0.
    clear_SDA();
    i2c_delay();
    clear_SCL();
    i2c_started = true;
}

void i2cBitBangingBus::i2c_stop_cond(void)
{
    // set SDA to 0
    clear_SDA();
    i2c_delay();
    // Clock stretching
    while (read_SCL() == 0)
    {
        // add timeout to this loop.
        i2c_sleep();
    }
    // Stop bit setup time, minimum 4us
    i2c_delay();
//  usleep(4);
    read_SDA();
    // SCL is high, set SDA from 0 to 1
    if (read_SDA() == 0)
    {
        arbitration_lost("i2c_stop_cond");
    }
    i2c_delay();
    i2c_started = false;
}

// Write a bit to I2C bus
void i2cBitBangingBus::i2c_write_bit(bool bit)
{
    if (bit)
    {
        read_SDA();
    }
    else
    {
        clear_SDA();
    }
    i2c_delay();
    while (read_SCL() == 0)
    { // Clock stretching
      // You should add timeout to this loop
        i2c_sleep();
    }
    // SCL is high, now data is valid
    // If SDA is high, check that nobody else is driving SDA
    if (bit && read_SDA() == 0)
    {
        arbitration_lost("i2c_write_bit");
    }
    i2c_delay();
    clear_SCL();
}

// Read a bit from I2C bus
bool i2cBitBangingBus::i2c_read_bit()
{
    bool bit;
    // Let the slave drive data
    read_SDA();
    i2c_delay();
    while (read_SCL() == 0)
    { // Clock stretching
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
bool i2cBitBangingBus::i2c_write_byte(bool send_start, bool send_stop, uint8_t byte)
{
    unsigned bit;
    bool nack;
    if (send_start)
    {
        i2c_start_cond();
    }
    for (bit = 0; bit < 8; bit++)
    {
        i2c_write_bit((byte & 0x80) != 0);
        byte <<= 1;
    }
    nack = i2c_read_bit();
    if (send_stop)
    {
        i2c_stop_cond();
    }
    return nack;
}

// Read a byte from I2C bus
uint8_t i2cBitBangingBus::i2c_read_byte(bool nack, bool send_stop)
{
    unsigned char byte = 0;
    unsigned bit;
    for (bit = 0; bit < 8; bit++)
    {
        byte = (byte << 1) | i2c_read_bit();
    }
    i2c_write_bit(nack);
    if (send_stop)
    {
        i2c_stop_cond();
    }
    return byte;
}

// KERNEL-LIKE I2C METHODS

// This executes the SMBus “write byte” protocol, returning negative errno else zero on success.
int32_t i2cBitBangingBus::i2c_smbus_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value)
{
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(true, false, address))
    {
        if (!i2c_write_byte(false, false, command))
        {
            if (!i2c_write_byte(false, true, value))
            {
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
int32_t i2cBitBangingBus::i2c_smbus_read_byte_data(uint8_t i2c_address, uint8_t command)
{
    uint8_t address = (i2c_address << 1) | 0;
    if (!i2c_write_byte(true, false, address))
    {

        if (!i2c_write_byte(false, false, command))
        {

            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(true, false, address))
            {
                return i2c_read_byte(true, true);
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
int32_t i2cBitBangingBus::i2c_smbus_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values)
{
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(true, false, address))
    {
        if (!i2c_write_byte(false, false, command))
        {
            bool errors = false;
            for (size_t i = 0; i < length; i++)
            {
                if (!errors)
                {
                    errors = i2c_write_byte(false, false, values[i]);
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
int32_t i2cBitBangingBus::i2c_smbus_read_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        uint8_t* values)
{
    uint8_t address = (i2c_address << 1) | 0;
    if (!i2c_write_byte(true, false, address))
    {

        if (!i2c_write_byte(false, false, command))
        {

            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(true, false, address))
            {
                for (uint8_t i = 0; i < length; i++)
                {
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

// Uncomment to compile with main method to test MPU 9X50
//#define TEST_MPU9250 1

#ifdef TEST_MPU9250  // Main Method to test MPU 9250
#include <signal.h>
#include <stdlib.h>

#define MPU_9150_I2C_ADDRESS_1          0x69    // Base address of the Drotek board
#define MPU_9150_I2C_ADDRESS_2          0x68    // Base address of the SparkFun board
#define MPU_9150_SMPRT_DIV              0x19    // Gyro sampling rate divider
#define MPU_9150_DEFINE                 0x1A    // Gyro and accel configuration
#define MPU_9150_GYRO_CONFIG            0x1B    // Gyroscope configuration
#define MPU_9150_ACCEL_CONFIG           0x1C    // Accelerometer configuration
#define MPU_9150_FIFO_EN                0x23    // FIFO buffer control
#define MPU_9150_INT_PIN_CFG            0x37    // Bypass enable configuration
#define MPU_9150_INT_ENABLE             0x38    // Interrupt control
#define MPU_9150_ACCEL_XOUT_H           0x3B    // Accel X axis High
#define MPU_9150_ACCEL_XOUT_L           0x3C    // Accel X axis Low
#define MPU_9150_ACCEL_YOUT_H           0x3D    // Accel Y axis High
#define MPU_9150_ACCEL_YOUT_L           0x3E    // Accel Y axis Low
#define MPU_9150_ACCEL_ZOUT_H           0x3F    // Accel Z axis High
#define MPU_9150_ACCEL_ZOUT_L           0x40    // Accel Z axis Low
#define MPU_9150_GYRO_XOUT_H            0x43    // Gyro X axis High
#define MPU_9150_GYRO_XOUT_L            0x44    // Gyro X axis Low
#define MPU_9150_GYRO_YOUT_H            0x45    // Gyro Y axis High
#define MPU_9150_GYRO_YOUT_L            0x46    // Gyro Y axis Low
#define MPU_9150_GYRO_ZOUT_H            0x47    // Gyro Z axis High
#define MPU_9150_GYRO_ZOUT_L            0x48    // Gyro Z axis Low
#define MPU_9150_USER_CTRL              0x6A    // User control
#define MPU_9150_PWR_MGMT_1             0x6B    // Power management 1

#define MPU_9150_I2C_MAGN_ADDRESS       0x0C    // Address of the magnetometer in bypass mode
#define MPU_9150_WIA                    0x00    // Mag Who I Am
#define MPU_9150_AKM_ID                 0x48    // Mag device ID
#define MPU_9150_ST1                    0x02    // Magnetometer status 1
#define MPU_9150_HXL                    0x03    // Mag X axis Low
#define MPU_9150_HXH                    0x04    // Mag X axis High
#define MPU_9150_HYL                    0x05    // Mag Y axis Low
#define MPU_9150_HYH                    0x06    // Mag Y axis High
#define MPU_9150_HZL                    0x07    // Mag Z axis Low
#define MPU_9150_HZH                    0x08    // Mag Z axis High
#define MPU_9150_ST2                    0x09    // Magnetometer status 2
#define MPU_9150_CNTL                   0x0A    // Magnetometer control

#define I2C_AUTO_INCREMENT  0x80




bool stopI2c = false;

void handleSigInt(int param)
{
    cout << "CTRL-C" << endl;
    stopI2c = true;
}

float valueToFloat(int16_t value)
{

    // -1.0 - 1.0
    if (value >= 0)
    {
        return static_cast<float>(value) /  static_cast<float>(SHRT_MAX);
    }
    else
    {
        return static_cast<float>(value) /  static_cast<float>(-SHRT_MIN);
    }
}

float valueToFloatPositive(int16_t value)
{

    // 0.0 - 1.0
    if (value >= 0)
    {
        return 0.5f + (static_cast<float>(value) / static_cast<float>(SHRT_MAX)) * 0.5f;
    }
    else
    {
        return 0.5f - (static_cast<float>(value) / static_cast<float>(SHRT_MIN)) * 0.5f;
    }
}

int main(void)
{
    // Register for Ctrl C from console
    signal(SIGINT, handleSigInt);

    wiringPiSetup();

    // Pin 0 = GPIO 17
    // Pin 2 = GPIO 27
    // Pin 8 = SDA.1
    // Pin 9 = SCL.1
    auto p0_p2 = make_shared<i2cBitBangingBus>(0, 2, 0);
    auto p8_p9 = make_shared<i2cBitBangingBus>(8, 9, 0);
    uint64_t count = 0;

    uint8_t block[6];

    auto address = MPU_9150_I2C_ADDRESS_1;
    auto bus = p0_p2;

    while (!stopI2c)
    {
        count++;
        try
        {
            if (count % 4 == 0)
            {
                cout << "\nBUS0_2 ADD1: ";
                address = MPU_9150_I2C_ADDRESS_1;
                bus = p0_p2;
            }
            else if (count % 4 == 1)
            {
                cout << "BUS8_9 ADD1: ";
                address = MPU_9150_I2C_ADDRESS_1;
                bus = p8_p9;
            }
            else if (count % 4 == 2)
            {
                cout << "BUS0_2 ADD2: ";
                address = MPU_9150_I2C_ADDRESS_2;
                bus = p0_p2;
            }
            else if (count % 4 == 3)
            {
                cout << "BUS8_9 ADD2: ";
                address = MPU_9150_I2C_ADDRESS_2;
                bus = p8_p9;
            }

            // 1 kHz sampling rate: 0b00000000
            if (bus->i2c_smbus_write_byte_data(address, MPU_9150_SMPRT_DIV, 0) < 0)
                throw runtime_error("I2C Write Error");
//            else
//                            cout << "value written " << endl;

//            auto value = p0_p2->i2c_smbus_read_byte_data(MPU_9150_I2C_ADDRESS_2, MPU_9150_SMPRT_DIV);
//            if (value < 0)
//                throw runtime_error("I2C Read Error");
////            else
////                cout << "value read: " << value << endl;

            // http://www.invensense.com/mems/gyro/documents/RM-MPU-9250A-00.pdf
            // seite 14/55
            float acc_scale = 2.0f; // 2G ist die Scala
            float gyro_scale = 250.0f; // 250 dps grad pro sekunde ist die Scala

            if ( bus->i2c_smbus_read_i2c_block_data( address, I2C_AUTO_INCREMENT | MPU_9150_ACCEL_XOUT_H, 6, block ) != 6 )
                throw runtime_error("i2c_smbus_read_i2c_block_data Error");

            cout << "ax: " << valueToFloat((int16_t)( block[0] << 8 | block[1] )) * acc_scale
                    << " ay: " << valueToFloat((int16_t)( block[2] << 8 | block[3] )) * acc_scale
                    << " az: " << valueToFloat((int16_t)( block[4] << 8 | block[5] )) * acc_scale
                    << endl;

            if ( bus->i2c_smbus_read_i2c_block_data( address, I2C_AUTO_INCREMENT | MPU_9150_GYRO_XOUT_H, 6, block ) != 6 )
                throw runtime_error("i2c_smbus_read_i2c_block_data Error");

//            cout << "gx: " << valueToFloat((int16_t)( block[0] << 8 | block[1] )) * gyro_scale
//                    << " gy: " << valueToFloat((int16_t)( block[2] << 8 | block[3] )) * gyro_scale
//                    << " gz: " << valueToFloat((int16_t)( block[4] << 8 | block[5] )) * gyro_scale
//                    << endl;


//            cout << count << endl;

        }
        catch (exception &ex)
        {
//            p0_p2->i2c_stop_cond();
            cout << "EX: " << ex.what() << endl;
        }
    }
    cout << count << endl;
    return 0;
}

#endif
