/*
 * i2cBitBangingBus.h
 *
 *  Created on: 06.03.2015
 *      Author: "Marek Wyborski"
 */

#ifndef I2C_BIT_BANG_H_
#define I2C_BIT_BANG_H_

#include <stdint.h>
#include <unistd.h>
#include <ctime>
#include <climits>

#include <string>

using namespace std;

class i2cBitBangingBus
    {
    private:
        uint8_t PIN_SDA;
        uint8_t PIN_SCL;
        uint32_t sleepTimeNanos;
        struct timespec nanoSleepTime;
        uint32_t delayTicks;
        bool i2c_started;

    public:
        i2cBitBangingBus(uint8_t pin_number_sda, uint8_t pin_number_scl, uint32_t sleepTimeNanos_ = 0,
                uint32_t delayTicks_ = 0);

        // I2C Methods
        // http://en.wikipedia.org/wiki/I%C2%B2C

    private:
        bool read_SCL(); // Set SCL as input and return current level of line, 0 or 1
        bool read_SDA(); // Set SDA as input and return current level of line, 0 or 1
        void clear_SCL(); // Actively drive SCL signal low
        void clear_SDA(); // Actively drive SDA signal low
        void arbitration_lost(string where);

        void i2c_sleep();
        void i2c_delay();

        void i2c_start_cond();
        void i2c_stop_cond();
        void i2c_write_bit(bool bit);
        bool i2c_read_bit();
        bool i2c_write_byte(bool send_start, bool send_stop, uint8_t byte);
        uint8_t i2c_read_byte(bool nack, bool send_stop);

    public:
        // KERNEL-LIKE I2C METHODS
        //#include <linux/i2c-dev.h> linux/i2c.h
        // source/drivers/i2c/i2c-core.c
        //https://www.kernel.org/doc/htmldocs/device-drivers/i2c.html

        // This executes the SMBus “write byte” protocol, returning negative errno else zero on success.
        int32_t i2c_smbus_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value);

        // This executes the SMBus “read byte” protocol, returning negative errno else a data byte received from the device.
        int32_t i2c_smbus_read_byte_data(uint8_t i2c_address, uint8_t command);

        // This executes the SMBus “block write” protocol, returning negative errno else zero on success.
        int32_t i2c_smbus_write_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length, const uint8_t * values);

        // This executes the SMBus “block read” protocol, returning negative errno else the number
        // of data bytes in the slave's response.
        int32_t i2c_smbus_read_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length, uint8_t* values);

    };

#endif /* I2C_BIT_BANG_H_ */
