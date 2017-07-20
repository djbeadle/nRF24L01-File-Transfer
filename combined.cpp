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

// For signal handler:
#include <csignal> 

// For stol:
#include <string>

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
static volatile int interrupt_flag = 0;
unsigned long startTime, stopTime, counter, rxTimer=0;

void interrupt_handler(int nothing)
{
	cout << "Ctrl-c pressed! Ending transmission and truncuating file.\n";
	interrupt_flag = 1;
}

void print_packet(uint8_t *pkt)
{
	printf("%d \"%s\"\n", (uint8_t*)pkt[0], (char*)pkt+1);
}

void print_re_tx_packet(uint8_t *re_tx_request)
{
	printf("Pkt id: %c\n", re_tx_request[1]);
	for(int i = 2; i < 32; i++)
	{
		if(re_tx_request[i] == 0)
		{
			printf("Found 0 at position: %d\n", i);
			break;
		}

		printf("%d: %d\n", i, re_tx_request[i]);
	}
	printf("returning\n");
	return;
}

void respond_special_pkt(uint8_t *packets, unsigned long &recv_pkts, uint32_t &total_pkts, int &highest_pkt_num, int hide, uint8_t *pkt_buf, FILE *output_file)
{
	uint8_t special_response[32];
	uint8_t buf[256];
	int buf_ptr = 0;
	cout << "Received a special packet!\n";
	printf("Received %d out of %d packets", recv_pkts, total_pkts);
	// Build an array with all the packets we're missing
	for(uint8_t i = 1; i < 255; i++)
	{
		if(packets[i] == 0)
		{
			buf[buf_ptr++] = i;
		}
	}
	printf("\nNum missing: %d/%d\n", buf_ptr, highest_pkt_num);
	// print out the packets we're missing
	// printf("missing: ");
	/* for(int i = 0; i< buf_ptr; i++)
	   {
	   printf("%d ", buf[i]);
	   }
	   printf("\n");*/
	// ask the transmitter to resend the packets we need.
	uint8_t re_tx_request[32];
	re_tx_request[0] = '\0'; //header
	re_tx_request[1] = '2';
	radio.stopListening();
	for(int i = 0; i < buf_ptr; i+=29)
	{
		// determine how many items 
		// we're copying and insert
		// a 0 at the proper place
		// at the end of the data
		int copy_qty = 0;
		if(buf_ptr - i > 29)
		{
			copy_qty = 29;
			re_tx_request[31] = 0;
		}	
		else
		{
			copy_qty = buf_ptr - i;
			// +2 to get past the '\0' and '2' in slots 0 and 1,
			re_tx_request[2 + copy_qty] = 0;
		}
		memcpy(re_tx_request+2, buf+i, copy_qty);
		if(hide != 1)
		{
			print_re_tx_packet(re_tx_request);
		}
		radio.write(&re_tx_request, sizeof(re_tx_request));
	}

	// pkt_id starts at 1, so the first
	// 30 bytes of pkt_buf are empty
	//
	// The very last packet in the filemay not be a full 30 bytes, but
	// we don't want to do strlen() on the entire pkt_buf to figure that
	// out. Hence the crazy math here. 
	int size = (highest_pkt_num-1)*30 + strnlen((char*)pkt_buf+(highest_pkt_num)*30, 30);
	printf("size of final packet: %d", size);
	fwrite(pkt_buf+30, sizeof(uint8_t), size, output_file);
	// reset the packet buffer
	memset(pkt_buf, '\0', 30 * 255);
	// reset the highest_pkt_num
	highest_pkt_num = 0;
	// reset the received array
	memset(packets, '0', sizeof(uint8_t) * 256);

	// Build and transmit the all clear
	// message telling the TX'er that
	// it's safe to continue. 
	uint8_t all_clear[32];
	for(int i = 0; i< sizeof(all_clear); i++)
	{
		all_clear[i] = '\0';
	}
	all_clear[0] = '\0';
	all_clear[1] = '4';
	radio.write(&all_clear, 32);
	radio.startListening();
}

void print_packet(uint8_t *pkt, FILE *file)
{
	fprintf(file, "\n!%d \"%s\"\n", (uint8_t*)pkt[0], (char*)pkt+1);
}

size_t getFilesize (const char* filename){
	struct stat st;
	if(stat(filename, &st) != 0){
		return 0;
	}
	return st.st_size;
}

int main(int argc, char** argv)
{
	signal(SIGINT, interrupt_handler);
	fstream *file;
	char *filename = argv[1];

	uint8_t packets[256];
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

		int filename_length = 32;
		char *save_name = (char*)malloc(filename_length);
		FILE *output_file;

		cout << "Please enter a file name up to " << filename_length << " characters in length\n";
		cout << "(This will overwrite any file with the same name)\n";
		cout << "> ";

		cin.getline(save_name, filename_length);
		output_file = fopen(save_name, "w");

		if(output_file == NULL)
		{
			cout << "Something weird happened trying to write to the file.\n";
			perror("The following error occurred: ");
			return 6;
		}

		// This array indiciates if we've received this respective
		// packet or not. 
		for(int i = 0; i < 256; i++){
			packets[i] = 0;
		}

		int start = 0; // Flow control
		unsigned long recv_pkts = 0; // the total # of data pkts we've received
		uint32_t filesize; 
		uint32_t total_pkts = 0; // total # of data pkts we're expecting, calculated from the filesize. 

		// packet buffer
		// Store every packet we receive in it's respective slot and then write them all as a unit. 
		int payload_bytes = 30;
		uint8_t *pkt_buf = (uint8_t*)malloc(payload_bytes * 255);
		int highest_pkt_num = 0; // Keeps track of our max place in the circular packet buffer
		memset(pkt_buf, '\0', payload_bytes * 255);

		// Receive all the packets: 
		while(start != 2 && interrupt_flag == 0)
		{
			while(start != 2 && radio.available() && interrupt_flag == 0)
			{
				radio.read(&data,32);

				// Read normal data packets:
				if((char*)data[0] != '\0')
				{
					uint8_t pkt_num = data[0];

					// Drop any packets we've already received (The ack must not have made it back to the sender)
					if(packets[pkt_num % 255] == true)
					{
						printf("Drop pkt: %d\n", pkt_num);
						continue;
					}

					// keep track of all the packets we've received in this set of 255
					packets[pkt_num % 255] = 1;
					highest_pkt_num = pkt_num > highest_pkt_num ? pkt_num : highest_pkt_num;
					if(hide != 1){
						// Uncomment the next line if you want to insert each packet number into the output
						// printf("(!%u!)", pkt_num);
						cout << (char*)data+1;
					}

					memcpy(pkt_buf + (30*(pkt_num%255)), data+1, strnlen((char*)data+1, 30));
					recv_pkts++;

				}
				// Respond to special packet:
				else if ((char*)data[0] == '\0' && (char)data[1] == '3')
				{
					respond_special_pkt(packets, recv_pkts, total_pkts, highest_pkt_num, hide, pkt_buf, output_file);
				}
				// Starting packet:
				else if((char*)data[0] == '\0' && (char)data[1] == '1')
				{
					cout << "File Transfer beginning!\n";
					fputs((char*)data, output_file);
					// The filesize is type uint32_t, which is 32 bits or 4 bytes. Hence the magic number 4. 
					memcpy(&filesize, data+2, 4);
					// Total_pkts is always off by one. Add one to fix. 
					total_pkts = filesize / 30 + 1;
					printf("Filesize: %d\n", filesize);
					printf("Total Pkts: %d\n", total_pkts);
					counter++;
					start = 1;
				}
				// Ending packet:
				else if ((char*)data[0] == '\0' && (char)data[1] == '9')
				{
					cout << "Recv'ed Ending Packet!\n";
					printf("Received %d out of %d packets", recv_pkts, total_pkts);
					start = 2;
					// print out the number of packets missing:
					int num_missing = 0;
					for(int i = 1; i <= highest_pkt_num; i++)
					{
						num_missing += (packets[i] == 1 ? 0 : 1);
						// printf("%d: %s\n", i, packets[i] ? "1" : "0");
					}
					printf("Num missing: %d/%d\n", num_missing, highest_pkt_num);
					// TODO: Get the missing packets
					
					respond_special_pkt(packets, recv_pkts, total_pkts, highest_pkt_num, hide, pkt_buf, output_file);
				}
				// Premature Termination:
				else if ((char*)data[0] == '\0' && (char)data[1] == '8')
				{
					cout << "\n Tranmission ended prematurely by the sender\n";
					start = 2;
					// print out the number of packets missing:
					uint8_t buf[255];
					uint8_t buf_ptr = 0;
					for(int i = 1; i <= highest_pkt_num; i++)
					{
						if(packets[i] == 0)
						{
							buf[buf_ptr++] = i;
						}
					}
					printf("Num missing: %d/%d\n", buf_ptr, highest_pkt_num);
				}
				// Everything else is junk. 
				else
				{
					cout << "Junk Packet\n";
					print_packet(data);
				}
			}
		}
		fclose(output_file);
		free(pkt_buf);
		free(save_name);
	} // file_rx loop
	else if (role == role_ping_out)
	{
		// Send the very first packet with filesize: 
		uint8_t first[32];
		for(int i = 0; i < 32; i++){
			first[i] = '\0';
		}
		first[1] = '1';
		// sprintf((char*)first+2, "%zd", getFilesize(filename));
		uint32_t filesize = getFilesize(filename);
		// the filesize is type uint32_t, which is 32 bits, or 4 bytes; 
		memcpy(first+2, &filesize, 4);
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
		while (eof == 0 && interrupt_flag == 0) {
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
				radio.startListening();

				// resend the packets the client asks for
				int all_clear = 0;
				while(all_clear == 0)
				{
					if(radio.available())
					{
						uint8_t data[32];
						memset(data, '\0', 32);
						radio.read(&data, 32);
						if(data[0] == '\0' && data[1] == '2')
						{
							cout << "print pkt\n";
							print_re_tx_packet(data);
						}
						if(data[0] == '\0' && data[1] == '4')
						{
							all_clear = 1;
							cout << "All clear received, continuing!\n";
						}
					}
				}
				radio.stopListening();
			}
			/* Drop some packets to simulate packet loss */
			/* if(special_ctr >= 10 && special_ctr <= 50)
			   {
			   for(int i = 1; i < 31; i++) {
			   file->get(code[i]);
			   if(*file == NULL) {
			   printf("Hit EOF!\n");
			   eof = 1;
			   code[i] = '\0';
			   break;
			   }
			   }
			   printf("dropped: %d ", special_ctr);
			   special_ctr++;
			   continue;
			   } */

			/* Transmit normal data packets */
			code[0] = special_ctr;
			for(int i = 1; i < 31; i++) {
				file->get(code[i]);
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

			long int cycles = 3;
			if(radio.write(&code,32) == false) {
				fprintf(stderr, "TX Failed: %d \"%s\"\n", code[0], code+1);
			}

			if(hide != 1)
			{
				cout << "Sent!\n";
			}
			if(!radio.txStandBy()) {
				counter+=3;
			}
			special_ctr++;

		} // read file loop

		// Send the very last packet
		uint8_t last[32];
		if(interrupt_flag == 1)
		{
			for(int i = 0; i < 32; i++){
				last[i] = '\0';
			}
			last[1] = '8';
			last[2] = 'P'; // Premature
			last[3] = 'E'; // End
			last[4] = 'o'; // of
			last[5] = 'T'; // Transmission

			radio.write(last, sizeof(last));
			sleep(1);
		}
		else 
		{
			for(int i = 0; i < 32; i++){
				last[i] = '\0';
			}
			last[1] = '9';
			radio.write(last, sizeof(last));
			sleep(1);
		}
	} // file_tx loop
} // main

