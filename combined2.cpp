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

/********************************
 * User Configurable Variables: *
********************************/

// Need to simulate some packet loss?
// Uncomment this to drop every packet where pkt_num % 25 == 0.
#define PKT_LOSS 25

// Sampling rate
const uint8_t measure_seconds = 4;

/***********************
 * Raspberry Pi Config *
************************/

// Radio CE Pin, CSN Pin, SPI Speed
// See http://www.airspayce.com/mikem/bcm2835/group__constants.html#ga63c029bd6500167152db4e57736d0939 and the related enumerations for pin information.

// Setup for GPIO 22 CE and CE0 CSN with SPI Speed @ 4Mhz
RF24 radio(RPI_V2_GPIO_P1_22, BCM2835_SPI_CS0, BCM2835_SPI_SPEED_4MHZ);

/*********************
 * System Variables: *
**********************/

// Radio pipe addresses for the 2 nodes to communicate.
const uint64_t addresses[2] = { 0xABCDABCD71LL, 0x544d52687CLL };

static volatile int interrupt_flag = 0;	// Catches Ctrl-c, for canceling transmisison
static volatile int timer_flag = 0; // For measuring transmission rate

// For determining how many packets we've received in this time interval when measuring reception rate
volatile uint16_t num_recvd_last = 0;

// These are details about how the packets are built.
// I guess if the radio's packet size changes you could change them, or if you wanted to increase the pkt_id from
// uint16_t to uint32_t at the expense of 2 data bytes you could do that oo. 
const int num_payload_bytes = 29; // 32 - 2 (header) - 1 (\0 at end) = 29
const int num_re_tx_payload_bytes = 26; // Round down to the nearest even number to keep math simple when dealing with 2 byte pkt ids. 
const int num_special_header_bytes = 2; // '\0' + some char 
const int num_header_bytes = 2; // sizeof(uint16_t) = 2
const int num_re_tx_header_bytes = 4; 

int hide = 1;

void interrupt_handler(int nothing)
{
	cin.clear();
	cout << "Ctrl-c pressed! Ending transmission and truncuating file.\n";
	interrupt_flag = 1;
}

uint8_t fletcher_8(uint8_t *data, size_t size)
{
	uint8_t sum1 = 0;
	uint8_t sum2 = 0;
	//printf("fletcher computation: \n");
	while(size--)
	{
	//	printf("i: %d, c: %c\n", size, *data);
		sum1+=*data++;
		sum2+= sum1;
	}
	return (sum1&0xF) | (sum2<<4);
}

void print_packet(uint8_t *pkt)
{
	printf("%d \"%s\"\n", (uint16_t*)pkt[0], (char*)pkt+num_payload_bytes);
}

uint16_t length_re_tx_packet(uint8_t *data)
{
	uint8_t ctr = 0;
	for(int i = num_re_tx_header_bytes; i < num_re_tx_header_bytes + num_re_tx_payload_bytes; i += sizeof(uint16_t))
	{
		uint16_t x; 
		memcpy(&x, data + i, sizeof(uint16_t));
		if(x == 0)
			return ctr;
		else
			ctr++;
	}
	return ctr;
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

bool contained_in_array(uint16_t id, uint16_t *array, uint8_t end)
{
	for(int i= 0; i < end; i++)
	{
		if(*(array + i * sizeof(uint16_t)) == id)
			return true;
	}
	return false;
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
int send_missing_pkts(uint8_t *pkt_buf)
{
	uint16_t num_expecting = 0; // number of re_tx pkts we're looking for
	uint16_t num_recvd = 0; // number of re_tx pkts we've actually received
	bool anything_recvd = 0; 
	uint8_t data[32];

	uint16_t *missing_pkts; // array of the packets we're missing
	uint16_t missing_pkts_loc = 0;

	int first = 0; 

	radio.startListening();
	uint32_t start = millis();
	// Wait 20 seconds for a response. If nothing comes in, presumably 
	// the receiver didn't need any packets re tx'ed and just quit. 
	while(interrupt_flag == 0 && anything_recvd == 0 || (num_recvd < num_expecting && millis() - start < 60000))
	{
		if(radio.available()){
			radio.read(&data, 32);
			
			if(data[0] == '\0' && data[1] == '2')
			{
				// Each re_tx pkt is uniquely ID'd by the first missing packet id it has. We don't want to add the same values to the missing_pkts array multiple times. 
				uint16_t pkt_id = 0;
				memcpy(&pkt_id, data + num_re_tx_header_bytes, sizeof(uint16_t));
				if(hide!=1) printf("Re_TX_request pkt_id: %d\n", pkt_id);
				if(first == 0){
					anything_recvd = 1;

					memcpy(&num_expecting, data+2, sizeof(uint16_t));
					if(hide!=1) printf("num_expecting: %d\n", num_expecting);
					missing_pkts = (uint16_t*)malloc(sizeof(uint16_t) * num_expecting*13);
					first =1;
				}
				else if(contained_in_array(pkt_id, missing_pkts, missing_pkts_loc) == true)
				{
					if(hide!=1) cout << "We've already seen this packet\n";
					continue;
				}
				else if(hide!=1) cout << "Haven't seen this packet before.\n";
				num_recvd++;

				int num_entries = length_re_tx_packet(data);
				if(hide!=1)
				{
					printf("num_expecting: %d\n", num_expecting);
					printf("num_recvd: %d\n", num_recvd);
					printf("num_entries: %d\n", num_entries);
				}

				uint16_t val= 0;

				for(int i = 0; i < num_entries; i++)
				{
					memcpy(&val, data+num_re_tx_header_bytes + i * sizeof(uint16_t), sizeof(uint16_t));
					if(hide!=1) printf("i: %d, val: %d\n", i, val);
					memcpy(&missing_pkts[missing_pkts_loc], &data[num_re_tx_header_bytes + i * sizeof(uint16_t)], sizeof(uint16_t)); 
					missing_pkts_loc++;
				}

				// if the RX'er doesn't need anything retransmitted num_recvd = 1 and num_expecting = 0
				if(num_recvd > num_expecting)
				{
					if(hide!=1) cout << "num_recvd >= num_expecting\n";
					return 1;
				}
			}
			else if(data[0] == '\0' && data[1] == '4')
			{
				cout << "Received the all clear signal\n";
				return 1;
			}	
			else
			{
				if(hide!=1) cout << "Don't recognize this type of packet!\n";
			}
		}
	}

	/* Re transmit all of the missing packets */
	cout << "Retransmitting dropped packets.\n";
	radio.stopListening();
	for(int i = 0; i < missing_pkts_loc; i++)
	{
		uint8_t data[32];
		memset(&data, '\0', 32);
		// uint16_t pkt_id = *(missing_pkts + (i * sizeof(uint16_t)));
		uint16_t pkt_id = missing_pkts[i];
	
		memcpy(&data, &pkt_id, sizeof(uint16_t));
		memcpy(&data[num_header_bytes], pkt_buf+(num_payload_bytes*pkt_id), num_payload_bytes);	
		
		uint8_t chk_sum = fletcher_8((uint8_t*)&data[num_header_bytes], num_payload_bytes);
		data[31] = chk_sum;

		if(hide!=1)
		{
			printf("Pkt_id: %d\n", pkt_id);
			printf("!:\" %s\"\n", data+num_header_bytes);
		}
	
		while(interrupt_flag == 0 && true)
		{
			if(radio.write(&data, 32) == false)
			{
				if(hide!=1)
				{
					cout << "Sending missing packet failed!\n";
				}
				usleep(50);
			}
			else
			{
				if(hide!=1)
				{
					cout << "Success! Missing packet sent!\n";
				}
				break;
			}
		}
	}
	free(missing_pkts);
	return 0;
}

void request_missing_pkts(uint8_t *pkt_buf, bool *recvd_array, uint16_t num_txed, int num_missing)
{
	// Build an array with all of the packets we're missing:
	uint16_t *missing;
	// TODO: Fix the missing array. I screwed up how the numbers are stored in it. 
	uint16_t missing_size = sizeof(uint16_t)*num_missing*2;
	if(hide!=1) printf("Size of all missing packet nums in bytes: %d\n", missing_size);
	missing = (uint16_t*)malloc(sizeof(uint16_t) * num_missing);
	uint16_t missing_loc = 0;

	for(uint16_t i = 1; i <= num_txed; i++)
	{
		if(recvd_array[i] == 0)
		{
			memcpy(&missing[missing_loc], &i, sizeof(uint16_t));
			missing_loc++;
		}
	}

	// Figure out how many packets we need to send the transmitter to
	// let it know what packets it needs to resend us. 
	int pkt_ids_per_pkt = num_re_tx_payload_bytes / sizeof(uint16_t);

	uint16_t num_re_tx_pkts = (missing_loc) / pkt_ids_per_pkt;
	if((missing_loc) % pkt_ids_per_pkt != 0)
		num_re_tx_pkts += 1;

	// Ask the transmitter to resend the packets we need
	uint8_t re_tx_pkt[32];
	if(missing_loc == 0)
	{
                // blast the packet for a max time of one minute
                cout << "Don't need any packets resent, attempting to send all clear to transmitter.\n";
                uint32_t time = millis();
                while(interrupt_flag == 0 && time - millis() <= 60000)
                {
                        if(radio.write(&re_tx_pkt, 32) == false)
                        {
                                if(hide!=1) cout<<"Sending all clear packet failed!\n";
                                else if(hide!=1)
                                {
                                        cout<<"Successfully sent all clear packet!\n";
					free(missing);
                                        return;
                                }
                        }
                }
                cout << "Sent all clear for one minute, no response. Presumabably the transmitter's ACKs are getting lost. Writing to file and exiting.\n";
		free(missing);
                return;
	}

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
		if(hide!=1) printf("Number of packets needed to convey missing packets to transmitter: %d\n", num_re_tx_pkts);
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

		if(hide != 1) printf("copy qty: %d\n", copy_qty);

		for(int t = 0; t < copy_qty; t++)
		{
			memcpy(&re_tx_pkt[num_re_tx_header_bytes + t*sizeof(uint16_t)], &missing[i+t], sizeof(uint16_t));
		}

		if(hide != 1) print_re_tx_packet(re_tx_pkt);

		// Blast the re_tx_pkt to the transmitter until we receive
		// an ACK in response
		if(hide != 1) cout << "Sending re_tx_pkt\n";

		radio.stopListening();
		while(interrupt_flag == 0)
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

		if(hide != 1) cout << "  Got a successful response!\n";
	}
	if(hide!=1) cout << "Returning\n";
	free(missing);
}

size_t getFilesize (const char* filename){
	struct stat st;
	if(stat(filename, &st) != 0){
		return 0;
	}
	return st.st_size;
}

void sigalrm_handler(int sig)
{
	timer_flag = true;
}

int main(int argc, char** argv)
{
	signal(SIGINT, interrupt_handler); // Ctrl-c interrupt handler
	fstream *file;
	uint8_t *packets; // buffer to store all of the packets
	char *filename = NULL;

	bool role_tx = 1, role_rx = 0;
	bool role = 0;

	bool measure = false;
	bool hide_progress_bar = false;

	bool z = false; // Flag to make sure we don't set both the -s and -d flags
	int c;
	while ((c = getopt (argc, argv, "s:d:nmhD")) != -1)
	{
		switch (c)
		{
			case 'D':
				hide = 0;
				break;
			case 'h':
				cout << "This is a simple wireless file transfer utility built for the nRF24 radio family!\n";
				cout << "It's built using TMRh20's C++ RF24 library, which can be found on Github:\n";
				cout << "https://github.com/nRF24/RF24";
				cout << "\n";
				cout << "Usage:\n";
				cout << "-h: Show this help text.\n";
				cout << "-s: The source file. Use this on the transmitter.\n";
				cout << "-d: The destination file. Use this on the receiver. It will overwrite any existing files.\n";
				cout << "-D: Show a bunch of debug messages. \n";
				cout << "-n: Hide the progress bar on the receiver. Use when measuring, if you like.\n";
				cout << "-m: Measure the successfull data reception rate. Doesn't count packets where checksums don't match\n";
				cout << "\n";
				cout << "Examples:\n";
				cout << "sudo ./combined2 -s ModernMajorGeneral.txt \n";
				cout << "sudo ./combined2 -d ModernMajorGeneral-recv.txt \n";
				break;	
			case 's': // Specify source file
				if(z == true)
				{
					cout << "Cannot be both transmitter and receiver!\n";
					return 25; 
				}
				filename = optarg;
				z = true;
				role = role_tx;
				break;
			case 'd': // Specify destination file
				if(z == true)
				{
					cout << "Cannot be both transmitter and receiver!\n";
					return 25;
				}
				filename = optarg;
				z = true;
				role = role_rx;
				break;
			case 'm': // Measure data reception rate
				measure = true;
				cout << "Measuring!\n";
				break;
			case 'n': // Hide the progress bar
				hide_progress_bar = true;
				cout << "Hiding progress bar!\n";
				break;
			case '?':
				fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				return 6;
		}

		/* for (int index = optind; index < argc; index++)
		{
			printf ("Non-option argument %s\n", argv[index]);
		} */
	}
	if (measure == true && role == role_tx)
	{
		cout << "ERROR: Cannot measure data reception rate from the transmitter.\n";
		return 6;
	}
	
	// Make sure the user specified a file. 
	if(z != true)
	{
		cout << "ERROR: At least one filename is required as an agrument. Use -s [source file] or -d [dest file]\n";
		return 6;
	}

	// Open the file
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
	radio.flush_tx();
	radio.flush_rx();
	radio.setChannel(110); 			// Channel choice can have a big effect on packet corruption. 
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_2MBPS);
	radio.setAutoAck(1);                     // Ensure autoACK is enabled
	radio.setRetries(1,1);                  // Optionally, increase the delay between retries & # of retries
	// Use 8 bit CRC for a slight performance benefit. 
	// If sender & receiver CRCs don't match, the sender & receiver won't be able to establish a connection. 
	radio.setCRCLength(RF24_CRC_8);

	if(hide == 0){
		radio.printDetails();
	}

	/************/
	/* RECEIVER */
	/************/
	if(role == role_rx)
	{
		if(measure == true)
		{
			signal(SIGALRM, &sigalrm_handler);
		}
		
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
		FILE *output_file;
		output_file = fopen(filename, "w");

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
		if(interrupt_flag != 0)
		{
			cout << "File transfer canceled by user.\n";
			return 6;
		}
		cout << "Waiting for transmission...\n";
		while(interrupt_flag == 0)
		{
			if(measure == true && timer_flag == true)
			{
				unsigned long recvd_this_interval = num_recvd - num_recvd_last;
				unsigned long rate_this_interval = recvd_this_interval / measure_seconds;
				int data_rate = rate_this_interval * num_payload_bytes;
				printf("Received %u pkts in %u seconds - %u pkts/sec - %d bytes/sec \n", recvd_this_interval, measure_seconds, recvd_this_interval / measure_seconds, data_rate);

				num_recvd_last = num_recvd;
				timer_flag = false;
				alarm(measure_seconds);
			}
			// Update our progress bar every 100 pkts
			if(hide_progress_bar == false && progress_ctr <= 0)
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
					cout << "\n";
					cout << "File transfer beginning.\n";
					memcpy(&filesize, data+num_special_header_bytes, 4);
					num_expected = filesize / num_payload_bytes; 
					// If filesize is not exactly divisible by
					// num_payload_bytes we need an extra packet
					if (filesize % num_payload_bytes != 0)
						num_expected += 1;
					printf("Filesize: %d\n", filesize);
					printf("Expected Pkts: %d\n", num_expected);
					// pkt_buf =(uint8_t*) calloc(num_expected, num_payload_bytes);
					pkt_buf = (uint8_t*) malloc((num_expected+1)* num_payload_bytes);
					memset(pkt_buf, '\0', (num_expected+1)*num_payload_bytes);
					// recvd_array = (bool*)calloc(num_expected, sizeof(bool));
					recvd_array = (bool*)malloc((num_expected+1)*sizeof(bool));
					control = 1;
					alarm(measure_seconds);
					continue;
				}
				/* Ending Packet */
				else if (control > 0 && (char)data[0] == '\0' && (char)data[1] == '\0' && (char)data[2] == '9')
				{
					if(hide!=1)
					{
						cout << "\nENDING PACKET\n";
						cout << "*****************\n";
						printf("* 0: %c\n", (char*)data[0]);
						printf("* 1: %c\n", (char*)data[1]);
						printf("* 2: %c\n", (char*)data[2]);
						printf("* 3: %c\n", (char*)data[3]);
						cout << "*****************\n";
					}
					uint16_t z_pkt_num = 0;
					memcpy(&z_pkt_num, &data, 2);
					uint16_t num_txed;
					memcpy(&num_txed, data+num_special_header_bytes, 2);
					if(hide!=1) printf("Received %d out of %d packets\n", num_recvd, num_expected);
					int num_missing = 0;
					for(int i = 1; i <= num_expected; i++)
					{
						num_missing += (*(recvd_array + (i * sizeof(bool))) == 1 ? 0 : 1);
					}
					cout << "\n";
					if(num_missing == 0)
						cout<<"No packet loss!\n";
					else
						printf("Missing %d packets, asking transmitter to resend them.\n", num_missing);

					if(num_missing ==0)
					{
						send_all_clear();
					}
					else
					{
						request_missing_pkts(pkt_buf, recvd_array, num_expected, num_missing);
						cout << "Ready to receive the missing packets:\n";
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

					// Drop any packets we've already
					// seen (The ACK we sent must not
					// have made it back to the sender
					if(recvd_array[pkt_num] == 1)
					{
						if(hide!=1) printf("Dropped Pkt: %d\n", pkt_num);
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
				uint16_t dsize = strlen((char*)pkt_buf+(highest_pkt_num*num_payload_bytes));
				int size = (highest_pkt_num-1)*num_payload_bytes + strnlen((char*)pkt_buf+(highest_pkt_num*num_payload_bytes), num_payload_bytes);
				printf("Received file size: %d\n", size);
				fwrite(pkt_buf+num_payload_bytes, sizeof(uint8_t), size, output_file);
				fclose(output_file);
				puts("Wrote to file!\n");
				send_all_clear();
				break;
			}
		}
		if(recvd_array != NULL)
		{
			free(recvd_array);
		}
		if(pkt_buf != NULL)
		{
			free(pkt_buf);
		}
	}
	/***************/
	/* TRANSMITTER */
	/***************/
	else if(role == role_tx)
	{
		radio.openWritingPipe(addresses[1]);
		radio.openReadingPipe(1,addresses[0]);
		radio.stopListening();
		// Send the very first packet with the filesize:
		uint8_t first[32];
		memset(&first, '\0', sizeof(first));
		first[1] = '1';
		uint32_t filesize = getFilesize(filename);
		if(filesize == 0)
		{
			cout << "Error: Will not transmit an empty file!\n";
			return 6;
		}
		memcpy(first+2, &filesize, 4);
		cout << "Attempting to establish connection...";
		cout.flush();
		while(interrupt_flag == 0)
		{
			if(radio.write(first, sizeof(first)) == false)
			{
				if(hide!=1) cout << "Sending first packet failed.\n";
			}
			else break;
		}
		if(interrupt_flag == 0)
		{
			cout << "Success!\n";
		}
		else
		{
			cout << "Attempt to establish a connection was canceled by the user.\n";
			return 6;
		}

		// Calculate the number of pkts we're TX'ing
		uint16_t total_num_pkts = filesize / num_payload_bytes; 
		// If filesize is not exactly divisible by
		// num_payload_bytes we need an extra packet
		if (filesize % num_payload_bytes != 0)
			total_num_pkts += 1;
		printf("Filesize: %d\n", filesize);
		printf("Total Number of Packets: %d\n", total_num_pkts);
		// Initalize the array we'll be transmitting
		char code[32];
		memset(&code, '\0', 32);

		// Things we'll need later:
		int eof = 0;
		uint16_t special_ctr = 1; // This IDs the data pkts. Starts at 1, 0 is reserved for special packets

		// Store all the packets in a buffer addressed by special_ctr
		// TODO: don't store entire file in memory, instead use fseek
		packets = (uint8_t*)malloc(num_payload_bytes * (total_num_pkts+1));

		cout << "Beginning Transmission.\n";
		// Read the entire file and store it into the packets array
		uint8_t chk_sum = 0;
		while(eof==0 && interrupt_flag == 0)
		{
			// Transmit normal data packets
			int checksum_loc = 0;
			memset(code, '\0', 32);
			memcpy(code, &special_ctr, 2);
			for(int i = 0 + num_header_bytes; i < 31; i++)
			{
				file->get(code[i]);
				if(*file==NULL)
				{
					if(hide!=1)
						cout << "Hit EOF!\n";
					eof = 1;
					break;
				}
			}
			memcpy(packets + (num_payload_bytes * special_ctr), &code[num_header_bytes], num_payload_bytes);

			chk_sum = fletcher_8((uint8_t*)&code[num_header_bytes], num_payload_bytes);
			code[31] = chk_sum;
			checksum_loc = 31;

			/* Simulate some packet loss for testing purposes */
			#ifdef PKT_LOSS
			if(special_ctr % 25 != 0)
			{	
			#endif
			if(radio.write(&code, 32))
			{
				if(hide != 1)
				{
					cout << "  Sent!\n";
					usleep(50);
				}
				else if(hide!=1)
					cout << "  Failed.\n";
			}
			#ifdef PKT_LOSS
		
			}
			#endif

			special_ctr++;
		}

		// Send the very last packet:
		uint8_t last[32];
		memset(&last, '\0', sizeof(last));
		if(interrupt_flag ==1)
		{
			last[2] = '8';

			radio.write(&last, sizeof(last));

		}
		else
		{
			if(hide!=1) printf("special_ctr: %d\n", special_ctr);
			last[2] = '9';
			int receiver_status = 0; 
			while(receiver_status == 0&& interrupt_flag == 0)
			{
			if(hide!=1) printf("Receiver status is: %d\n", receiver_status);
			while(true)
			{
				if(radio.write(&last, sizeof(last)) ==  false)
				{
					if(hide!=1) cout << "Final packet TX failed\n";
				}
				else
				{
					if(hide!=1) cout << "Final packet sent!\n";
					break;
				}
			}
			if(interrupt_flag == 0)
			{
				cout << "Getting list of dropped packets\n";
				receiver_status = send_missing_pkts(packets);
			}
			}
			if(interrupt_flag == 0)
			{
				cout << "File transfer looks successful!\n";
			}
			else
			{
				cout << "File transfer was canceled by user.\n";
			}
			sleep(1);
		}
		free(packets);
	} // tx block
	radio.closeReadingPipe(addresses[0]);
	radio.closeReadingPipe(addresses[1]);
	radio.powerDown();
} // main
