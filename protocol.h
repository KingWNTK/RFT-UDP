#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <arpa/inet.h>
#include <iostream>
using namespace std;

#define CONTROL_META_DATA 1
#define CONTROL_FIN 2
#define CONTROL_FIN_ACK 4


#define FIN_RETRANS_CD 5
#define CLOCK_TICK_MICROS 100

#define SEQ_MAX 255

struct packet_header {
    unsigned int seq_num : 8;
    unsigned int control : 8;
    unsigned int length : 16;
    unsigned int checksum: 16;
};

void print_header(packet_header *header) {
    cout << "seq_num: " << header->seq_num << ", control: " << header->control
        <<", length: " << header->length << ", checksum: " << header->checksum << endl; 
}

void to_network_format(packet_header *header) {
    header->length = (unsigned int)htons(header->length);
    header->checksum = (unsigned int)htons(header->checksum);
}

void to_host_format(packet_header *header) {
    header->length = (unsigned int)ntohs(header->length);
    header->checksum = (unsigned int)ntohs(header->checksum);
}

unsigned short cal_sum(void *data, int dsize) {
    unsigned int sum = 0;
    int p = 0;
    while(dsize > 0) {
        if(dsize == 1) {
            sum += *(char *)(data + p);
        }
        else {
            sum += *(unsigned short *)(data + p);
        }
        if(sum & 0xFFFF0000) {
            sum &= 0xFFFF;
            sum++;
        }
        p += 2;
        dsize -= 2;
    }
    return (unsigned short)(sum & 0xFFFF);
}

unsigned short gen_checksum(void *data, unsigned int dsize) {
    return ~cal_sum(data, dsize);
}

bool verify_checksum(unsigned int checksum, void *data, unsigned int dsize) {
    return cal_sum(data, dsize) == (unsigned int)0xFFFF;
}

bool sanity_check(void *packet, unsigned int packet_len) {
    auto header = (packet_header *)packet;
    to_host_format(header);
    return verify_checksum(header->checksum, packet, packet_len);
}


#endif