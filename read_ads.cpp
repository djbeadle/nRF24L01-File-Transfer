
/*
 * This is a test to read values from the ads1115
 * using the wiring pi drivers
 */

#include <wiringPi.h>
#include <ads1115.h>
#include <stdio.h>
#include <stdint.h>
#include <ctime>

#include <iostream>

// comment out this line to disable the Readings Per Second printout
#define rps 0

int main(void)
{
	int16_t x, y, z;
	// std::chrono::time_point<std::chrono::system_clock> now;

	ads1115Setup(100, 0x48);
	// Set the sample rate. This is the fastest. 
	digitalWrite(101, 6);

	int ctr = 0;

#ifdef rps	
	while(true)
	{
		time_t start = time(0);

		while((uintmax_t)time(0) - start < 10)
		{
#endif

#ifndef rps
		while(true)
		{
#endif
			x = (int16_t)analogRead(100);
			y = (int16_t)analogRead(101);
			z = (int16_t)analogRead(102);

			// printf("%s %d %d %d\n",std::ctime(&now), x, y, z);
			std::cout /*<< ms.count()*/ << " " << x << " " << y << " " << z << "\n";
			//fflush(stdout);
			ctr++;
		}
#ifdef rps
		printf("Readings in one second: %i\n", ctr/10);
		ctr = 0;
		delay(1000);
	}
#endif
		return 0;
	}
