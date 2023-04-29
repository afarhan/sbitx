#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <linux/types.h>
#include <stdint.h>

#define SI570_ADDRESS 0x55

static int fd_si570;
static	unsigned char dco_reg[13];

#define RFREQ_37_32_MASK	0x3f
#define FDCO_MIN	4800000000.0
#define FDCO_MAX	5670000000.0
//#define fxtal 114292961
static unsigned int selected_n1;
static unsigned int selected_hs;
static uint64_t selected_rf = 0;
static unsigned long	selected_freq = 0;

int fxtal = 114264401;

void si570_dumpregs(){
	for (int i = 7; i <= 12; i++)
		printf("%d: %02x\n", i, dco_reg[i]);
}

void si570_read(){
	for (int i=7; i <= 12; i++)
		dco_reg[i] = wiringPiI2CReadReg8(fd_si570, i);
}

void si570_write(){

	int freeze_reg  = wiringPiI2CReadReg8(fd_si570, 137);
	wiringPiI2CWriteReg8(fd_si570, 137, freeze_reg | 0x10); 

	for(int i = 7; i <= 12; i++)
		wiringPiI2CWriteReg8(fd_si570, i, dco_reg[i]);

	
	wiringPiI2CWriteReg8(fd_si570, 137, freeze_reg & 0xef); 
	wiringPiI2CWriteReg8(fd_si570, 135, 0x40); 
}


/*
Set registers. 
How it works

	Output Freq = (fractional multiplier * internal crystal freq) / 
								(HS * N1)

	The Si570 is a PLL running between 4.5 GHz and 5.5 Ghz appox. The exact 
	frequency is of the PLL is set by the fractional multiplier. 
	This is divided down by HS and N1 to a value in the 160 MHz to 10 MHz range.
	There are three basic things to get about programming the Si570.

	1. The fractional multiplier is stored as an integer with a virtual decimal 
	after	the 28th bit. Thus to generate the integer representation from the fraction
	you must multiply the fraction with 2^28 = 268,435,456 and vice versa.

	2. The two successive dividers that bring the GHz PLL down to VHF/HF range are
	stored a little oddily too. The HS can only take divider values of 4,6,7,9 and 	11 
	(no 8 and 10 allowed). The HS is stored a 3 bit number with 4 subtracted
	from the actual value of the HS. Only even values are allowed for N1 though the 
	actual value stored in the register is off by one. Thus all the stored values of 
	N1 are odd but effectively the division is by the next even integer.

	Here is an example:
	Let's assume that the internal crystal value is 114.236,576 Mhz (a common value).
	Let's imagine that we want the Si570 to generate 14.000,000 MHz.
		1. We search a number of combinations of HS and N1 that allow a frequency 
		between 4.8 Ghz and 5.67 GHz that can be divided down to 14 MHz. We must search
		for the lowest N1 and the highest HS. Such a combination is HS=11, N1 =32 
		As, 32 x 11 x 14 MHz = 5.082 GHz
		2. Now, we calculate the fraction required to multiply the fxtal to 5.082 Ghz.
		rfreq = (fdco/fxtal) = 5.082 ghz / 114.236576 Mhz = 44.4866275.
		3. Now, we convert this to the requird format:
		HS = 11 - 4 = 0x7
		N1 = 32 - 1 = 0x1f
		RFREQ = 44.4866275 * 268,435,456 = 11941788138 = 0x2c7c939ea 02 c7 c9 39 ea
		
		Now, this is packed inside registers 7 to 12

		Reg 7: HS1(2) 	HS1(1) 	HS1(1) 	N1(6) 	N1(5) 	N1(4) 	N1(3) 	N1(2)
		Reg 8: N1(1) 		N1(0) 	RF(37)	RF(36)	RF(35)	RF(34)	RF(33)	RF(32)
		Reg 9: RF(31-24)
		Reg10: RF(23-16)
		Reg11: RF(15-8)
		Reg12: RF(7-0)

	We flip bit 5 of reg 135 to avoid changing the freq while the PLL is running.


*/
void si570_freq(unsigned long f){
  unsigned int hs, n1;
  float f_dco;

//	printf("si570: %ld\n", f);
	for (hs = 11; hs >= 4; hs--){
		if (hs == 8 || hs == 10)	//hs can't take values of 8 and 10 as per datasheet
			continue;

		// n1 = fdco / (hs * fout), we are taking care of not rounding out the significant bits	
		n1 = (long)(FDCO_MIN/hs)/f;
		if (!n1 || (n1 & 1))					//n1 should not be zero or odd
			n1++;	
		while (n1 <= 128){
			f_dco = (float)f  * (float)hs * (float)n1;
			if (f_dco > FDCO_MAX) {
				break;
			}
			
      if (FDCO_MIN < f_dco && f_dco < FDCO_MAX){
				selected_n1 = n1;
				selected_hs = hs;
				selected_rf = (uint64_t) ((f_dco /fxtal) * 268435456.0);

				//fill up the registers
				dco_reg[7] 	= (selected_hs - 4) << 5;
				dco_reg[7] 	= dco_reg[7] | ((selected_n1 - 1) >> 2);
				dco_reg[8] 	= ((selected_n1 - 1) & 0x3) << 6 | ((selected_rf >> 32) & RFREQ_37_32_MASK);
				dco_reg[9] 	= (selected_rf >> 24) & 0xff;
				dco_reg[10] =	(selected_rf >> 16) & 0xff;
				dco_reg[11] = (selected_rf >> 8) & 0xff;
				dco_reg[12] = selected_rf & 0xff;  
				si570_write();
				selected_freq = f;
				return;
      }
			n1 += (n1 == 1 ? 1 : 2);
  	}
	}
}


void si570_init(){
	fd_si570 = wiringPiI2CSetup(SI570_ADDRESS);
	si570_read();
}

/*
int main(int argc, char **argv){
	unsigned long f;

	if (argc != 2){
			puts("Usage: si570 [freq in hz]");
			return -1; 
	}

	f = atol(argv[1]);
	
	si570_init();
	for (int i = 7; i <= 12; i++)
		printf("%d: %02x\n", i, dco_reg[i]);

	si570_freq(f);
	si570_dumpregs();
} 
*/
