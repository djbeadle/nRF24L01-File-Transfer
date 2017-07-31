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

static volatile int interrupt_flag = 0;

const int num_payload_bytes = 29; // 32 - 2 (header) - 1 (\0 at end) = 29
const int num_re_tx_payload_bytes = 26; // Round down to the nearest even number to keep math simple when dealing with 2 byte pkt ids. 
const int num_special_header_bytes = 2; // '\0' + some char 
const int num_header_bytes = 2; // sizeof(uint16_t) = 2
const int num_re_tx_header_bytes = 4; 

int hide = 0;

void interrupt_handler(int nothing)
{
	cout << "Ctrl-c pressed! Ending transmission and truncuating file.\n";
	interrupt_flag = 1;
}

void print_packet(uint8_t *pkt)
{
	printf("%d \"%s\"\n", (uint16_t*)pkt[0], (char*)pkt+num_payload_bytes);
}

void print_re_tx_packet(uint8_t *re_tx_request)
{
	printf("**************\n");
	printf("* Pkt id: %c\n", re_tx_request[1]);
	printf("* Num re_tx_pkts: %d\n", re_tx_request[2]);
	for(int i = 4; i < num_re_tx_header_bytes + num_re_tx_payload_bytes; i+=sizeof(uint16_t))
	{
		uint16_t val;
		memcpy(&val, re_tx_request+i, sizeof(uint16_t));
		if(val == 0)
		{
			printf("* Found 0 at position: %d\n", i);
			break;
		}
		printf("val: %d\n", val);
	}
	printf("**************\n");
	return;
}

/* There are no missing packets we need retransmitted, so we'll send an empty re_tx packet to the TX'er */
void send_all_clear()
{
	uint8_t data[32];
	memset(&data, '\0', 32);
	data[1] = '2';

	// Hopefully the TX'er received the message. 
	// Since we have everything, it isn't the end
	// of the world if it doesn't. Since it exits after
	// sending the ACK there's  no point in us trying to 
	// send the all clear signal forever, though. 
	radio.stopListening();
	radio.write(&data, 32);
	radio.startListening();
}


void request_missing_pkts(uint8_t *pkt_buf, bool *recvd_array, uint16_t num_txed, int num_missing)
{
	printf("Missing %d  packets\n", num_missing);
	// Build an array with all of the packets we're missing:
	uint16_t *missing;
	// TODO: Fix the missing array. I screwed up how the numbers are stored in it. 
	uint16_t missing_size = sizeof(uint16_t)*num_missing*2;
	printf("Size of missing: %d\n", missing_size);
	missing = (uint16_t*)malloc(sizeof(uint16_t) * num_missing);
	uint16_t missing_loc = 0;

	// TODO: GET RID OF FILE PRINTING DEBUGGING HERE. 	
	// ofstream missing_pkts_file;
//	missing_pkts_file.open("missing_packets.txt");
	for(uint16_t i = 1; i <= num_txed; i++)
	{
//		missing_pkts_file << "i: " << i << ", recvd_array[i]: " << recvd_array[i] << "\n";
		if(recvd_array[i] == 0)
		{
			// *(missing + (missing_loc * sizeof(uint16_t))) = i;
			memcpy(&missing[missing_loc], &i, sizeof(uint16_t));
			missing_loc++;
		}
	}
//	missing_pkts_file.close();

	// Figure out how many packets we need to send the transmitter to
	// let it know what packets it needs to resend us. 
	int pkt_ids_per_pkt = num_re_tx_payload_bytes / sizeof(uint16_t);

	uint16_t num_re_tx_pkts = (missing_loc) / pkt_ids_per_pkt;
	if((missing_loc) % pkt_ids_per_pkt != 0)
		num_re_tx_pkts += 1;

	// Ask the transmitter to resend the packets we need
	uint8_t re_tx_pkt[32];

	for(int i=0; i < missing_loc; i+=pkt_ids_per_pkt)
	{
		if(hide!=1)
		{
			printf("i: %d\n", i);
		}

		// Build the re_tx packet
		memset(&re_tx_pkt[0], '\0', 32);
		re_tx_pkt[0] = '\0';
		re_tx_pkt[1] = '2';
		printf("num_re_tx_pkts: %d\n", num_re_tx_pkts);
		memcpy(&re_tx_pkt[2], &num_re_tx_pkts, 2);

		// determine how many pkt ids we're putting into this re_tx
		// pkt and insert a \0 at the proper place
		int copy_qty;
		if(missing_loc - i > pkt_ids_per_pkt)
		{
			copy_qty = pkt_ids_per_pkt;
			re_tx_pkt[30] = '\0';
		}
		else
		{
			copy_qty = missing_loc - i; 
			re_tx_pkt[num_re_tx_header_bytes + (sizeof(uint16_t) * copy_qty)] = '\0';
		}

		if(hide != 1)
			printf("copy qty: %d\n", copy_qty);

		for(int t = 0; t < copy_qty; t++)
		{
			memcpy(&re_tx_pkt[num_re_tx_header_bytes + t*sizeof(uint16_t)], &missing[i+t], sizeof(uint16_t));
		}

		if(hide != 1)
			print_re_tx_packet(re_tx_pkt);

		// Blast the re_tx_pkt to the transmitter until we receive
		// an ACK in response
		cout << "1\n";
		if(hide != 1)
			cout << "Sending re_tx_pkt\n";

		radio.stopListening();
		while(true)
		{
			if(radio.write(&re_tx_pkt, sizeof(re_tx_pkt)))
				break;
			else
			{
				if(hide!=1)
				{
					cout << " sending re_tx_pkt failed\n";
				}
				radio.startListening();
				if(radio.available())
				{
					cout << "TX'er has sent us something. That must mean it thinks it knows all of the missing packets.\n";
					break;
				}
				radio.stopListening();
			}
		}
		radio.startListening();

		if(hide != 1)
			cout << "  Got a successful response!\n";
	}
	if(hide!=1)
		cout << "Returning\n";
	free(missing);
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

	uint8_t *packets; // buffer to store all of the packets
	if(argc == 3)
	{
		if (strcmp("-h", argv[2]) == 0)
			hide = 1;
		file = new fstream(filename, fstream::in);
	}
	else if(argc == 2)
		file = new fstream(filename, fstream::in);

	if(file == NULL)
	{
		cout << "Could not open the file.\n";
		return 6;
	}


	/*******************************/
	/* PRINT PREAMBLE AND GET ROLE */
	/*******************************/
	if(hide == 0)
		cout << "RF24/examples/combined2.cpp\n";

	radio.begin();                           // Setup and configure rf radio
	radio.setChannel(6);
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_2MBPS);
	radio.setAutoAck(1);                     // Ensure autoACK is enabled
	radio.setRetries(4,15);                  // Optionally, increase the delay between retries & # of retries
	radio.setCRCLength(RF24_CRC_16);          // Use 8-bit CRC for performance
	if(hide == 0){
		radio.printDetails();
		printf("\n ************ Role Setup ***********\n");
	}

	/* ROLE CHOOSER */
	bool role_tx = 1, role_rx = 0;
	bool role = 0;

	string input = "";
	char myChar = {0};
	cerr << "Choose a role: Enter 0 for receiver, 1 for transmitter (CTRL+C to exit)\n>";
	getline(cin,input);

	if(input.length() == 1) {
		myChar = input[0];
		if(myChar == '0') {
			cout << "Role: Pong Back, awaiting transmission " << endl << endl;
			role = role_rx;
		} else {
			cout << "Role: Ping Out, starting transmission " << endl << endl;
			role = role_tx;
		}
	}

	/************/
	/* RECEIVER */
	/************/
	if(role == role_rx)
	{
		/* Things we will need later: */
		uint32_t filesize = 0;
		uint32_t num_expected = 0; // # of pkts we're expecting
		unsigned long num_recvd = 0; // # of pkts actually recved
		uint16_t highest_pkt_num = 0;
		uint8_t *pkt_buf; // Store every pkt before writing it.
		bool *recvd_array; // Keep track of which slots in the pkt_buf array have been written to

		float progress = 0.0;
		float progress_inc = 0;
		int progress_ctr = 100;
		int bar_width = 70;

		/* Open a file for writing to */
		int filename_length = 32;
		char *save_name = (char*) malloc(filename_length);
		FILE *output_file;

		cout << "Please enter a file name up to " << filename_length << "characters in length\n";
		cout << "(This will overwrite any file with the same name)\n";
		cout << "> ";
		cin.getline(save_name, filename_length);
		output_file = fopen(save_name, "w");

		if(output_file == NULL)
		{
			cout << "Something weird happened trying to write to the file\n";
			perror("The following error occurred: ");
			return 6;
		}

		radio.openWritingPipe(addresses[0]);
		radio.openReadingPipe(1,addresses[1]);
		radio.startListening();
		/* Packet RX Loop: */
		uint8_t data[32];
		/* 
		 * Control flag:
		 * 0 - have not received starting packet
		 * 1 - starting packet received, ready for data pkts
		 */
		int control = 0; 
		while(interrupt_flag == 0)
		{
			// Update our progress bar every 100 pkts
			if(progress_ctr <= 0)
			{
				float normalized_progress = (float)(num_recvd - 1)/(float)(num_expected - 1);
				cout << "[";
				int pos = bar_width * normalized_progress;
				for(int i = 0; i < bar_width; ++i)
				{
					if (i<pos) cout << "=";
					else if (i==pos) cout << ">";
					else cout << " ";
				}
				cout << "]" << int(normalized_progress*100.0) << " %\r";
				cout.flush();
				progress_ctr =100;
			}
			uint8_t data[32];
			if(radio.available())
			{
				// cout << "control: " << control << "\n";
				radio.read(&data, 32);
				/* Receive the starting packet with our file size */
				if(control == 0 && (char*)data[0] == '\0' && (char)data[1] == '1')
				{
					cout << "File transfer beginning!\n";
					memcpy(&filesize, data+num_special_header_bytes, 4);
					num_expected = filesize / num_payload_bytes; 
					// If filesize is not exactly divisible by
					// num_payload_bytes we need an extra packet
					if (filesize % num_payload_bytes != 0)
						num_expected += 1;
					if(hide != 1)
					{
						printf("Filesize: %d\n", filesize);
						printf("Expected Pkts: %d\n", num_expected);
					}
					// pkt_buf =(uint8_t*) calloc(num_expected, num_payload_bytes);
					pkt_buf = (uint8_t*) malloc((num_expected+1)* num_payload_bytes);
					memset(pkt_buf, '\0', (num_expected+1)*num_payload_bytes);
					// recvd_array = (bool*)calloc(num_expected, sizeof(bool));
					recvd_array = (bool*)malloc((num_expected+1)*sizeof(bool));
					control = 1; 
					continue;
				}
				/* Ending Packet */
				else if (control > 0 && (char)data[0] == '\0' && (char)data[1] == '\0' && (char)data[2] == '9')
				{
					cout << "ENDING PACKET\n";
					cout << "*****************\n";
					printf("* 0: %c\n", (char*)data[0]);
					printf("* 1: %c\n", (char*)data[1]);
					printf("* 2: %c\n", (char*)data[2]);
					printf("* 3: %c\n", (char*)data[3]);
					cout << "*****************\n";
					uint16_t z_pkt_num = 0;
					memcpy(&z_pkt_num, &data, 2);
					printf("If this was a data pkt it's pkt_num would be: %d\n", z_pkt_num);
					uint16_t num_txed;
					memcpy(&num_txed, data+num_special_header_bytes, 2);
					printf("TX'er said it sent %d packets\n", num_txed);
					printf("\nReceived %d out of %d packets\n", num_recvd, num_expected);
					int num_missing = 0;
					for(int i = 1; i <= num_expected; i++)
					{
						num_missing += (*(recvd_array + (i * sizeof(bool))) == 1 ? 0 : 1);
					}
					printf("num_missing: %d\n", num_missing);

					if(num_missing ==0)
					{
						send_all_clear();
					}
					else
					{
						request_missing_pkts(pkt_buf, recvd_array, num_expected, num_missing);
						cout << "Looping to receive the missing packets:\n";
					}
					control = 3;
				}
				/* Receive data packets */
				// else if(control > 0 && data[0] != '\0')
				else
				{
					uint16_t pkt_num;
					memcpy(&pkt_num, data, 2);
					num_recvd++;
					// printf("P: %d\n", pkt_num);

					// Drop any packets we've already
					// seen (The ACK we sent must not
					// have made it back to the sender
					if(recvd_array[pkt_num] == 1)
					{
						printf("Dropped Pkt: %d\n", pkt_num);
						continue;
					}
					if(hide != 1)
					{
						printf("pkt_num: %d, num_recvd: %d, num_expected: %d\n", pkt_num, num_recvd, num_expected);
						printf("\"%s\"\n", (char*)data+num_header_bytes);
					}

					// Properly keep track of new pkts
					*(recvd_array + (pkt_num * sizeof(bool))) = 1;
					memcpy(pkt_buf + (pkt_num * num_payload_bytes), data + num_header_bytes, strnlen((char*)data + num_header_bytes, num_payload_bytes));
					highest_pkt_num = (pkt_num > highest_pkt_num) ? pkt_num : highest_pkt_num;
					progress_ctr--;
				}
			}
			/* Check and see if we have everything! */
			if(control == 3 && num_expected == num_recvd)
			{
				cout << "a\n";	
				uint16_t dsize = strlen((char*)pkt_buf+(highest_pkt_num*num_payload_bytes));
				int size = (highest_pkt_num-1)*num_payload_bytes + strnlen((char*)pkt_buf+(highest_pkt_num*num_payload_bytes), num_payload_bytes);
				cout << "b\n";	
				printf("size: %d\n", size);
				fwrite(pkt_buf+num_payload_bytes, sizeof(uint8_t), size, output_file);
				puts("closing\n");
				fclose(output_file);
				puts("Wrote to file!\n");
				break;
			}
		}
		
		// TODO: GET RID OF FILE PRINTING DEBUGGING HERE. 
		ofstream missing_pkts_file;
		missing_pkts_file.open("missing_packets.txt");
		for(uint16_t i = 1; i <= highest_pkt_num; i++)
		{
			missing_pkts_file << "i: " << i << ", recvd_array[i]: " << recvd_array[i] << "\n";
		}
	}
	/***************/
	/* TRANSMITTER */
	/***************/
	else if(role == role_tx)
	{
		if(argc == 1){
			cerr << "Transmit mode requires a filename as an argument!\n";
			return 6;
		}
		radio.openWritingPipe(addresses[1]);
		radio.openReadingPipe(1,addresses[0]);
		radio.stopListening();
		cout << "TX'ing\n";
		// Send the very first packet with the filesize:
		uint8_t first[32];
		memset(&first, '\0', sizeof(first));
		first[1] = '1';
		uint32_t filesize = getFilesize(filename);
		memcpy(first+2, &filesize, 4);
		radio.write(first, sizeof(first));

		// Calculate the number of pkts we're TX'ing
		uint16_t total_num_pkts = filesize / num_payload_bytes; 
		// If filesize is not exactly divisible by
		// num_payload_bytes we need an extra packet
		if (filesize % num_payload_bytes != 0)
			total_num_pkts += 1;
		if(hide != 1)
		{
			printf("Filesize: %d\n", filesize);
			printf("Total Number of Packets: %d\n", total_num_pkts);
		}
		// Initalize the array we'll be transmitting
		char code[32];
		memset(&code, '\0', 32);

		// Things we'll need later:
		int eof = 0;
		uint16_t special_ctr = 1; // This IDs the data pkts. Starts at 1, 0 is reserved for special packets

		// Store all the packets in a buffer addressed by special_ctr
		// TODO: don't store entire file in memory, instead use fseek
		uint8_t *packets = (uint8_t*)malloc(num_payload_bytes * total_num_pkts);

		// Read the entire file and store it into the packets array
		while(eof==0 && interrupt_flag == 0)
		{
			// Transmit normal data packets
			memcpy(code, &special_ctr, 2);
			for(int i = 0 + num_header_bytes; i < 31; i++)
			{
				file->get(code[i]);
				if(*file==NULL)
				{
					if(hide!=1)
						cout << "Hit EOF!\n";
					eof = 1;
					code[i] = '\0';
					break;
				}
			}
			memcpy(packets + (num_payload_bytes * special_ctr), code, num_payload_bytes);

			code[31] = '\0';

			// Confirm the contents of the packet:
			if(hide != 1)
			{
				uint16_t pkt_num;
				memcpy(&pkt_num, code, 2);
				printf("pkt: %d \"%s\"\n", pkt_num, code+num_header_bytes);
			}

			if(radio.write(&code, 32))
			{
				if(hide != 1)
					cout << "  Sent!\n";
				else 
					if(hide!=1)
						cout << "  Failed.\n";
			}

			special_ctr++;
		}

		// Send the very last packet:

		uint8_t last[32];
		memset(&last, '\0', sizeof(last));
		if(interrupt_flag ==1)
		{
			last[1] = '8';
			last[1] = 'P'; // Premature
			last[1] = 'E'; // End
			last[1] = 'o'; // of
			last[1] = 'T'; // Transmission

			radio.write(&last, sizeof(last));

		}
		else
		{
			// The final pkt is transmitted and special counter is incremented. 
			printf("Last pkt #: %d\n", special_ctr-1);
			last[1] = '9';
			while(true)
			{
				if(radio.write(&last, sizeof(last)) ==  false)
				{
					cout << "Final packet TX failed\n";
				}
				else
				{
					cout << "Final packet sent!\n";
					break;
				}
			}
			sleep(1);
		}
	} // tx block
} // main
