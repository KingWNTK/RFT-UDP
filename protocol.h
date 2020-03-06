#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <arpa/inet.h>

#define CONTROL_META_DATA 1


struct packet_header {
    unsigned int seq_num : 8;
    unsigned int control : 8;
    unsigned int length : 16;
    unsigned int checksum: 16;
};

void to_network_format(packet_header *header) {
    header->length = (unsigned int)htons(header->length);
    header->checksum = (unsigned int)htons(header->checksum);
}

void to_host_format(packet_header *header) {
    header->length = (unsigned int)ntohs(header->length);
    header->checksum = (unsigned int)ntohs(header->checksum);
}

unsigned short cal_sum(unsigned short sum, void *data, unsigned int dsize) {
    while(dsize > 0) {
        if(dsize == 1) {
            sum += ((unsigned short)*(char *)data) << 16;
        }
        else {
            sum += *(unsigned short *)data;
        }
        if(sum & 0xFFFF0000) {
            sum &= 0xFFFF;
            sum++;
        }
        dsize -= 2;
    }
    return sum;
}

unsigned short gen_checksum(void *data, unsigned int dsize) {
    return ~(cal_sum(0, data, dsize) & 0xFFFF);
}

bool verify_checksum(unsigned short checksum, void *data, unsigned int dsize) {
    return cal_sum(checksum, data, dsize) == 0;
}

bool sanity_check(void *packet, unsigned int packet_len) {
    auto header = (packet_header *)packet;
    return verify_checksum((unsigned short)header->checksum, packet, packet_len);
}


#endif