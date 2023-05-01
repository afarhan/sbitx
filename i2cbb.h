/*
 * i2cBitBangingBus.h
 *
 * Based on Mark Wyborski's work, re-written for C by Ashhar Farhan, VU2ESE
 */

void i2cbb_init(uint8_t pin_number_sda, uint8_t pin_number_scl);

// This executes the SMBus “write byte” protocol, returning negative errno else zero on success.
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value);

// This executes the SMBus “read byte” protocol, returning negative errno 
// else a data byte received from the device.
int32_t i2cbb_read_byte_data(uint8_t i2c_address, uint8_t command);

// This executes the SMBus “block write” protocol, returning negative errno else zero on success.
int32_t i2cbb_write_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length, 
	const uint8_t * values);

// This executes the SMBus “block read” protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2cbb_read_i2c_block_data (uint8_t i2c_address, uint8_t length, uint8_t* values);

