/*
The MIT License (MIT)

Copyright (c) 2015 silverx

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


// Eachine H8mini acro firmware
// files of this project are assumed MIT licence unless otherwise noted

// licence of files binary.h and macros.h LGPL 2.1


#include "gd32f1x0.h"

#include "config.h"
#include "led.h"
#include "util.h"
#include "sixaxis.h"
#include "drv_adc.h"
#include "drv_time.h"
#include "drv_softi2c.h"
#include "drv_pwm.h"
#include "drv_adc.h"
#include "drv_gpio.h"
#include "drv_rgb.h"
#include "drv_serial.h"
#include "rx_bayang.h"
#include "drv_spi.h"
#include "control.h"
#include "pid.h"
#include "defines.h"
#include "drv_i2c.h"
#include "buzzer.h"
#include "binary.h"
#include <math.h>
#include "hardware.h"

#include <inttypes.h>

#ifdef USE_SERIAL_4WAY_BLHELI_INTERFACE
#include "drv_softserial.h"
#include "serial_4way.h"
#endif




#ifdef __GNUC__
// gcc warnings and fixes
#ifdef AUTO_VDROP_FACTOR
//	#undef AUTO_VDROP_FACTOR
//	#warning #define AUTO_VDROP_FACTOR not working with gcc, using fixed factor
#endif
#endif



// hal
void clk_init(void);

float looptime;

void failloop(int val);

unsigned long maintime;
unsigned long lastlooptime;

int ledcommand = 0;
int ledblink = 0;

unsigned long ledcommandtime = 0;


int lowbatt = 0;
float vbatt = 4.2;
float vbattfilt = 4.2;
float vbatt_comp = 4.2;
int random_seed = 0;
float vref = 1.0;
float vreffilt = 1.0;

extern char aux[AUXNUMBER];

extern int rxmode;
extern int failsafe;

extern void loadcal(void);
extern void imu_init(void);

#ifdef USE_SERIAL_4WAY_BLHELI_INTERFACE
volatile int switch_to_4way = 0;
static void setup_4way_external_interrupt(void);
#endif

int main(void)
{

	clk_init();

	gpio_init();

	time_init();

	i2c_init();

	spi_init();

	pwm_init();

  pwm_dir(FREE);

//	bridge_sequencer(DIR1);

	for (int i = 0; i <= 3; i++)
	  {
		  pwm_set(i, 0);
	  }



	if (RCC_GetCK_SYSSource() == 8)
	  {

	  }
	else
	  {
		  failloop(5);
	  }

	sixaxis_init();

	if (sixaxis_check())
	  {
#ifdef SERIAL
		  printf(" MPU found \n");
#endif
	  }
	else
	  {
#ifdef SERIAL
		  printf("ERROR: MPU NOT FOUND \n");
#endif
		  failloop(4);
	  }

	adc_init();


// loads acc calibration and gyro dafaults
// also autobind , pids
	loadcal();

	rx_init();

	int count = 0;
	vbattfilt = 0.0;

	while (count < 64)
	  {
		  vbattfilt += adc_read(ADC_ID_VOLTAGE);
		  count++;
	  }
       // for randomising MAC adddress of ble app - this will make the int = raw float value
		random_seed =  *(int *)&vbattfilt ;
		random_seed = random_seed&0xff;

	vbattfilt = vbattfilt / 64;

#ifdef SERIAL
	printf("Vbatt %2.2f \n", vbattfilt);
#ifdef NOMOTORS
	printf("NO MOTORS\n");
#warning "NO MOTORS"
#endif
#endif

#ifdef STOP_LOWBATTERY
// infinite loop
	if (vbattfilt < STOP_LOWBATTERY_TRESH)
		failloop(2);
#endif


	gyro_cal();

	rgb_init();

#ifdef SERIAL_DRV
	serial_init();
#endif

	imu_init();

	extern unsigned int liberror;
	if (liberror)
	  {
#ifdef SERIAL
		  printf("ERROR: I2C \n");
#endif
		  failloop(7);
	  }


	lastlooptime = gettime();


	float thrfilt = 0;

//
//
//              MAIN LOOP
//
//


	checkrx();


#ifdef USE_SERIAL_4WAY_BLHELI_INTERFACE
	setup_4way_external_interrupt();
#endif


	while (1)
	  {
		  // gettime() needs to be called at least once per second
		  maintime = gettime();
		  looptime = ((uint32_t) (maintime - lastlooptime));
		  if (looptime <= 0)
			  looptime = 1;
		  looptime = looptime * 1e-6f;
		  if (looptime > 0.02f)	// max loop 20ms
		    {
			    failloop(3);
			    //endless loop
		    }
		  lastlooptime = maintime;

		  if (liberror > 20)
		    {
			    failloop(8);
			    // endless loop
		    }

		  sixaxis_read();

		  control();

// battery low logic
        // read battery voltage
        vbatt = adc_read(ADC_ID_VOLTAGE);
#ifdef ADC_ID_REF
        // account for vcc differences
        vbatt = vbatt/vreffilt;
        // read reference to get vcc difference
        vref = adc_read(ADC_ID_REF);
        // filter reference
        lpf ( &vreffilt , vref , 0.9968f);
#endif
		float hyst;

		// average of all 4 motor thrusts
		// should be proportional with battery current
		extern float thrsum; // from control.c

		// filter motorpwm so it has the same delay as the filtered voltage
		// ( or they can use a single filter)
		lpf ( &thrfilt , thrsum , 0.9968f);	// 0.5 sec at 1.6ms loop time

        static float vbattfilt_corr = 4.2;
        // li-ion battery model compensation time decay ( 18 seconds )
        lpf ( &vbattfilt_corr , vbattfilt , FILTERCALC( 1000 , 18000e3) );

        lpf ( &vbattfilt , vbatt , 0.9968f);


// compensation factor for li-ion internal model
// zero to bypass
#define CF1 0.25f

        float tempvolt = vbattfilt*( 1.00f + CF1 )  - vbattfilt_corr* ( CF1 );

#ifdef AUTO_VDROP_FACTOR

static float lastout[12];
static float lastin[12];
static float vcomp[12];
static float score[12];
static int z = 0;
static int minindex = 0;
static int firstrun = 1;


if( thrfilt > 0.1f )
{
	vcomp[z] = tempvolt + (float) z *0.1f * thrfilt;

	if ( firstrun )
    {
        for (int y = 0 ; y < 12; y++) lastin[y] = vcomp[z];
        firstrun = 0;
    }
	float ans;
	//	y(n) = x(n) - x(n-1) + R * y(n-1)
	//  out = in - lastin + coeff*lastout
		// hpf
	 ans = vcomp[z] - lastin[z] + FILTERCALC( 1000*12 , 6000e3) *lastout[z];
		lastin[z] = vcomp[z];
		lastout[z] = ans;
	 lpf ( &score[z] , ans*ans , FILTERCALC( 1000*12 , 60e6 ) );
	z++;

	if ( z >= 12 ) z = 0;

    float min = score[0];

    if (z == 11)
    {
        for ( int i = 0 ; i < 12; i++ )
        {
         if ( (score[i]) < min )
            {
                min = (score[i]);
                minindex = i;
                // add an offset because it seems to be usually early
                minindex++;
            }
        }
    }

}

#undef VDROP_FACTOR
#define VDROP_FACTOR  minindex * 0.1f
#endif

		if ( lowbatt ) hyst = HYST;
		else hyst = 0.0f;

		if ( tempvolt + (float) VDROP_FACTOR * thrfilt <(float) VBATTLOW + hyst )
            lowbatt = 1;
		else lowbatt = 0;

        vbatt_comp = tempvolt + (float) VDROP_FACTOR * thrfilt;

// led flash logic

		  if (rxmode != RX_MODE_BIND)
		    {
					// non bind
			    if (failsafe)
			      {
				      if (lowbatt)
					      ledflash(500000, 8);
				      else
					      ledflash(500000, 15);
			      }
			    else
			      {
				      if (lowbatt)
					      ledflash(500000, 8);
				      else
					{
						if (ledcommand)
						  {
							  if (!ledcommandtime)
								  ledcommandtime = gettime();
							  if (gettime() - ledcommandtime > 500000)
							    {
								    ledcommand = 0;
								    ledcommandtime = 0;
							    }
							  ledflash(100000, 8);
						  }

						else if (ledblink)
						{
							unsigned long time = gettime();
							if (!ledcommandtime)
							{
								  ledcommandtime = time;
								  ledoff( 255);
							}
							if (time - ledcommandtime > 500000)
							    {
								    ledblink--;
								    ledcommandtime = 0;
							    }
							if ( time - ledcommandtime > 300000)
							{
								ledon( 255);
							}
						}

						else
						{
							if ( aux[LEDS_ON] )
							ledon( 255);
							else
							ledoff( 255);
						}
					}
			      }
		    }
		  else
		    {		// bind mode
			    ledflash(100000 + 500000 * (lowbatt), 12);
		    }

// rgb strip logic
#if (RGB_LED_NUMBER > 0)
	extern void rgb_led_lvc( void);
	rgb_led_lvc( );
#endif

#ifdef BUZZER_ENABLE
	buzzer();
#endif

#ifdef FPV_ON
			static int fpv_init = 0;
			if ( rxmode == RX_MODE_NORMAL && ! fpv_init ) {
				fpv_init = gpio_init_fpv();
			}
			if ( fpv_init ) {
				if ( failsafe ) {
					GPIO_WriteBit( FPV_PIN_PORT, FPV_PIN, Bit_RESET );
				} else {
					GPIO_WriteBit( FPV_PIN_PORT, FPV_PIN, aux[ FPV_ON ] ? Bit_SET : Bit_RESET );
				}
			}
#endif

	checkrx();

#ifdef USE_SERIAL_4WAY_BLHELI_INTERFACE
	extern int onground;
	if (onground)
	{
		NVIC_EnableIRQ(EXTI4_15_IRQn);

		if (switch_to_4way)
		{
			switch_to_4way = 0;

			NVIC_DisableIRQ(EXTI4_15_IRQn);
			ledon(2);
			esc4wayInit();
			esc4wayProcess();
			NVIC_EnableIRQ(EXTI4_15_IRQn);
			ledoff(2);

			lastlooptime = gettime();
		}
	}
	else
	{
		NVIC_DisableIRQ(EXTI4_15_IRQn);
	}
#endif


	// loop time 1ms
	while ( gettime() - maintime < 1000 - 1 )
	{}



	}			// end loop


}

// 2 - low battery at powerup - if enabled by config
// 3 - loop time issue
// 4 - Gyro not found
// 5 - clock , intterrupts , systick
// 7 - i2c error
// 8 - i2c error main loop

void delay3( int x)
{
  x>>=8;
    for( ; x> 0 ; x--)
    {
      #ifdef BUZZER_ENABLE
      failsafe = 1;
      buzzer();
      #endif
      delay(256);
    }
}

void failloop(int val)
{
	for (int i = 0; i <= 3; i++)
	  {
		  pwm_set(i, 0);
	  }

    delay(1000000);

	while (1)
	  {
		  for (int i = 0; i < val; i++)
		    {

			    ledon(255);
			    delay3(200000);
			    ledoff(255);
			    delay3(200000);
		    }
		  delay3(800000);
	  }

}



void HardFault_Handler(void)
{
	failloop(5);
}

void MemManage_Handler(void)
{
	failloop(5);
}

void BusFault_Handler(void)
{
	failloop(5);
}

void UsageFault_Handler(void)
{
	failloop(5);
}


#ifdef USE_SERIAL_4WAY_BLHELI_INTERFACE

// set up external interrupt to check
// for 4way serial start byte
static void setup_4way_external_interrupt(void)
{
	SYSCFG->EXTISS[3] &= ~(0x000F) ; //clear bits 3:0 in the SYSCFG_EXTICR1 reg
	EXTI->FTE |= EXTI_FTE_FTE14;
	EXTI->IER |= EXTI_IER_IER14;
	NVIC_SetPriority(EXTI4_15_IRQn,2);
}

// interrupt for detecting blheli serial 4way
// start byte (0x2F) on PA14 at 38400 baud
void EXTI4_15_IRQHandler(void)
{
	if( (EXTI->IER & EXTI_IER_IER14) && (EXTI->PD & EXTI_PD_PD14))
	{
#define IS_RX_HIGH (GPIOA->DIR & GPIO_PIN_14)
		uint32_t micros_per_bit = 26;
		uint32_t micros_per_bit_half = 13;

		uint32_t i = 0;
		// looking for 2F
		uint8_t start_byte = 0x2F;
		uint32_t time_next = gettime();
		time_next += micros_per_bit_half; // move away from edge to center of bit

		for (; i < 8; ++i)
		{
			time_next += micros_per_bit;
			delay_until(time_next);
			if ((0 == IS_RX_HIGH) != (0 == (start_byte & (1 << i))))
			{
				i = 0;
				break;
			}
		}

		if (i == 8)
		{
			time_next += micros_per_bit;
			delay_until(time_next); // move away from edge

			if (IS_RX_HIGH) // stop bit
			{
				// got the start byte
				switch_to_4way = 1;
			}
		}

		// clear pending request
		EXTI->PD |= EXTI_PD_PD14 ;
	}
}
#endif

