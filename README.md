
The nRF24L01+ is a low power 2.4GHz transciever made by Nordic Semiconductor.
This is a file transfer utility optimized for the small (32 byte!) packet size supported by these radios.

Packet Anatomy:
======

There are a couple different types of packets.

#### First Packet:
~~~~
0            1          2                   6      32
*------------*----------*-------------------*------*
| uint8_t 0 | uint8_t 1 | uint32_t filesize | null |
*------------*----------*-------------------*------*
    1 byte      1 byte         4 bytes       26 bytes
~~~~

The receiver calculates how many packets it is expecting from the filesize.

#### Data Packet:
~~~~
0                    2             31                 32
*--------------------*-------------*------------------*
| uint16_t Packet ID | Packet Data | uint8_t checksum |
*--------------------*-------------*------------------*
        2 bytes         29 bytes          1 byte
~~~~

The nRF24L01+ supports hardware checksums and auto re-transmit, but in practice I haven't found them to be reliable enough for file transfer. This simply uses the fletcher 8-bit checksum. Collisions are possible, but I haven't witnessed it yet. *Which means it's not a problem, right? Right...?*

### Special Packet:
A "special packet" asks the receiver what packets it's missing. It's pretty simple.

~~~~
0                                                                 32
*-----------------------------------------------------------------*
| 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 |
*-----------------------------------------------------------------*
                            32 bytes
~~~~
## Example

A typical file transfer looks like this:

~~~~
Transmitter             Receiver
|                           |
|-------------Ready?----->  |
| <-----------Yep!----------|
|                           |
|---------Data Pkt #1---->  |
| <----Auto Ack #1----------|
|                           |
|---------Data Pkt #2---->  |
|        Dropped! <---Ack---|
| Dropped! <----------Ack---|
|    Dropped! <-------Ack---|
|                           |
|---------Data Pkt #2---->  |
|         Dropped! <--Ack---|
|        Dropped! <---Ack---|
|         Dropped! <--Ack---|
~~~~

At this point transmitting packet #2 has failed, so the transmitter moves on and tries to transmit the rest of the file.
When the transmitter has sent every packet at least once, it sends a "special packet"

~~~~
|---------Special Pkt---->  |
| <----Auto Ack ------------|
~~~~

The receiver responds with a "ReTransmit Request" packet. These packets list all of the packets it is missing, or had bad software calculated checksums. 

~~~~
| <--ReTransmit Request #1--|
|---------Ack-------------> |
| <--ReTransmit Request #2--|
|---------Ack-------------> |
| <--ReTransmit Request #3--|
|---------Ack-------------> |
~~~~

Once the receiver has everything, it retransmits all the missing packets!

~~~~
|---------Data Pkt #2---->  |
| <----Auto Ack #2----------|
~~~~

And then another "special packet".
~~~~
|---------Special Pkt---->  |
| <----Auto Ack ------------|
~~~~

If the receiver has everything, it responds with an "all clear" packet and we're done!

~~~~
| <---------All Clear!------|
|-----------Auto Ack------> |
~~~~

If the receiver is missing something, it sends back some more retransmit requests, and the process repeats until the receiver has everything.

Compile command for wiringPi c code:
g++ -Wall -o read_ads read_ads.cpp -lwiringPi -std=c++11

Read ADS:
./read_ads

Transmit:
sudo ./combined [filename]
or: 
sudo ./combined [filename] -h

Receive:
sudo ./combined
or: 
sudo ./combined -h

Plot data in terminal (Using a package called 'feedgnuplot' to send data to gnuplot):
cat out.t | feedgnuplot --terminal 'dumb 80, 24' --exit
