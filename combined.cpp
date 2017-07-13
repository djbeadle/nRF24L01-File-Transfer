/*
   TMRh20 2014

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as published by the Free Software Foundation.
 */

/** General Data Transfer Rate Test
 * This example demonstrates basic data transfer functionality with the
 updated library. This example will display the transfer rates acheived using
 the slower form of high-speed transfer using blocking-writes.
 */

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <RF24/RF24.h>
#include <unistd.h>

// For stat: 
#include <sys/stat.h>
#include <sys/types.h>


using namespace std;

//
// Hardware configuration
//

/****************** Raspberry Pi ***********************/

// Radio CE Pin, CSN Pin, SPI Speed
// See http://www.airspayce.com/mikem/bcm2835/group__constants.html#ga63c029bd6500167152db4e57736d0939 and the related enumerations for pin information.

// Setup for GPIO 22 CE and CE0 CSN with SPI Speed @ 4Mhz
RF24 radio(RPI_V2_GPIO_P1_22, BCM2835_SPI_CS0, BCM2835_SPI_SPEED_4MHZ);

/**************************************************************/

// Radio pipe addresses for the 2 nodes to communicate.
const uint64_t addresses[2] = { 0xABCDABCD71LL, 0x544d52687CLL };

uint8_t data[32];
unsigned long startTime, stopTime, counter, rxTimer=0;

size_t getFilesize (const char* filename){
	struct stat st;
	if(stat(filename, &st) != 0){
		return 0;
	}
	return st.st_size;
}

int main(int argc, char** argv)
{
	fstream *file;
	char *filename = argv[1];
	int hide = 0;
	if(argc == 3)
	{
		if(strcmp("-h", argv[2]) == 0)
		{
			hide = 1;
		} 
		// fstream file(argv[1], fstream::in);
		file = new fstream(filename, fstream::in);
	}
	else if (argc == 2)
	{
		file = new fstream(filename, fstream::in);
	}

	if(file == NULL)
	{
		cerr << "Could not open the file.\n";
		return 6;
	}

	bool role_ping_out = 1, role_pong_back = 0;
	bool role = 0;

	// Print preamble:
	if(hide == 0){
		cout << "RF24/examples/combined.cpp/\n";
	}

	radio.begin();                           // Setup and configure rf radio
	radio.setChannel(1);
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_2MBPS);
	radio.setAutoAck(1);                     // Ensure autoACK is enabled 
	radio.setRetries(4,15);                  // Optionally, increase the delay between retries & # of retries
	radio.setCRCLength(RF24_CRC_8);          // Use 8-bit CRC for performance
	if(hide == 0){
		radio.printDetails();
		printf("\n ************ Role Setup ***********\n");
	}
	/********* Role chooser ***********/
	string input = "";
	char myChar = {0};
	cerr << "Choose a role: Enter 0 for receiver, 1 for transmitter (CTRL+C to exit)\n>";
	getline(cin,input);

	if(input.length() == 1) {
		myChar = input[0];
		if(myChar == '0') {
			cout << "Role: Pong Back, awaiting transmission " << endl << endl;
		} else {
			cout << "Role: Ping Out, starting transmission " << endl << endl;
			role = role_ping_out;
		}
	}
	/***********************************/

	if ( role == role_ping_out )
	{
		if(argc == 1){
			cerr << "Transmit mode requires a filename as an argument!\n";
			return 6;
		}
		radio.openWritingPipe(addresses[1]);
		radio.openReadingPipe(1,addresses[0]);
		radio.stopListening();
	}
	else
	{
		radio.openWritingPipe(addresses[0]);
		radio.openReadingPipe(1,addresses[1]);
		radio.startListening();
	}

	// file_rx loop
	if(role == role_pong_back) {
		while(1)
		{
			while(radio.available()) {
				radio.read(&data,32);
				cout << (char*)data;
				counter++;
			}
		}
	} // file_tx loop
	else if (role == role_ping_out)
	{
		// Send the very first packet with filesize: 
		uint8_t first[32];
		for(int i = 0; i < 32; i++){
			first[i] = '\0';
		}
		first[1] = '1';
		sprintf((char*)first+2, "%zd", getFilesize(filename));
		radio.write(first, sizeof(first));

		// forever loop
		char code[32];
		for(int i = 0; i< sizeof(code); i++)
		{
			code[i] = '\0';
		}
		int eof = 0;
		int ctr = 0; // the number of times we've read 31 characters from the file
		
		// Special_ctr is the packet id #.
		// It starts at 1, packets starting with 0 are reserved for special control packets
		uint8_t special_ctr = 1;
		// Read the entire file
		while (eof == 0) {
			// Ask the receiver to confirm receipt of the last 255 packets
			// No comparison of checksums or anything, the receiver just checks to see if it has received every packet, and if it hasn't it asks for for them to be resubmitted. 
			// Why 255? Because max(uint8_t) == 255
			if(special_ctr == 255)
			{
				// cout << "Special Packet time!\n";
				special_ctr = 1;
				uint8_t special[32];
				for(int i = 0; i< 32; i++)
				{
					special[i] = '\0';
				}
				special[1] = '3';
				radio.write(special, sizeof(special));
			/*	string temp = "";
				cout << "Press Enter to continue...\n";
				getline(cin, temp);*/
			}

			code[0] = special_ctr;
			for(int i = 1; i < 31; i++) {
			//	printf("i: %d\n", i);
				file->get(code[i]);
			//	printf("%d \"%s\"\n", code[0], code+1);
				if(*file == NULL) {
					printf("Hit EOF!\n");
					eof = 1;
					code[i] = '\0';
					break;
				}
			}
			code[31] = '\0';
			if(hide != 1)
			{
				printf("pkt: %d \"%s\"\n", code[0], code+1);
			}
			ctr++;

			// printf("Initiating Basic Data Transfer\n\r");

			long int cycles = 3;
			while(true){
				if(radio.write(&code,32) == false) {
				fprintf(stderr, "resending pkt: %d \"%s\"\n", code[0], code+1);
				}
				else
				{
					break;
				}
			}
			/*
			while(radio.writeFast(&code,32) == false) {
			}
			*/
	
			if(hide != 1)
			{
				cout << "Sent!\n";
			}
			if(!radio.txStandBy()) {
				counter+=3;
			}
			special_ctr++;
			
		//	usleep(6000);
		} // read file loop

		// Send the very last packet with filesize: 
		uint8_t last[32];
		for(int i = 0; i < 32; i++){
			last[i] = '\0';
		}
		last[1] = '9';
		radio.write(last, sizeof(last));
		sleep(1);
	} // file_tx loop
} // main

