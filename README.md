# Reliable file transportation on UDP

# How to run
Simply use make to make both files, and you can run the programs with the following form:

`sendfile -r <recvhost>:<recvport> -f <subdir>/<filename>`  
`recvfile -p <recvport>`  

# Protocol
Everything of the protocol is inside the protocol.h file and we'll have an overview of the protocol.  
First, the packet format:

```c
struct packet_header {  
    unsigned int seq_num : 16;  
    unsigned int control : 8;  
    unsigned int length : 16;  
    unsigned int checksum: 16;  
    unsigned int start;  
}; 
```

seq_num describes the current packet sequence number, it serves as ack number in packets sent from receiver.

control is one of the following values or 0:
```c
CONTROL_SYNC
CONTROL_FILEPATH
CONTROL_FIN
CONTROL_FIN_ACK
```
These values tell that current packet has some control functions, such as its a sync packet before start  transmitting, it contains a filepath of the file to be transmitted, and it is fin packet or ack of fin packet.  

length represents the packet length

checksum represents the 1's compliment checksum of the whole packet

start represents the start position of this packet regarding to the whole file under transmission.

There also are some values defined in the protocol to control the whole transmission process:
```c
CLOCK_TICK_MICROS
PACKET_DSIZE_MAX
SEQ_MAX
```
These values tell the clock speed, maximum packet data size, and maximum sequence number. 

# design highlight
To implement the timeout retransmission scheme, we decide to utilize multi threading. A clock thread consistantly sleep for `CLOCK_TICK_MICROS` microseconds. When its waken up, it will acquire a mutex lock to update every packet in the sliding window. When we find out that we've been waiting for a packet for a certain time, we should retransmit it.  

We've noticed that if the network condition is bad, i.e. high delay rate, a packet from prior transmission might affect the later one causing the program to crush. To address this issue, we mimic the way TCP handles it.

First, we'll do three-way handshake, the sendfile will send a sync packet to the recvfile, and recvfile send a sync with a randomnized starting sequence number back, and finally the sendfile send a packet containing the file path to indicate the link is successfully established.

After everything is transmitted, sendfile will send a fin packet with the sequence number of the last transmitted data packet, and once recvfile has received this packet, it will send back a fin packet with the same sequence number. Finally, sendfile will send a fin ack packet indicating it's ready to close. If the recvfile didn't here anything from sendfile for a long time at this moment, it knows that the fin ack is lost, and it should close connection now.

Our design gaurantees that consecutive transmissions will not affect each other, thus provide a very realiable file transmission approach.





