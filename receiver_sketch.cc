#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>
#include <deque>
#include <pthread.h>
#include <string>
#include <iostream>

#include "protocol.h"

using namespace std;

struct win_entry {
    int seq_num;
    char *data;
    int dsize;
    int control;
    win_entry(int seq_num_in, char *data_in, int dsize_in, int control_in) : seq_num(seq_num_in), data(data_in), dsize(dsize_in), control(control_in) {
    }
    bool is_empty() {
        return (!data);
    }
    set_data(char *data_in, int dsize_in) {
        dsize = dsize_in;
        data = (char *)malloc(dsize);
        memcpy(data, data_in, dsize);
    }
    ~win_entry() {
        if (data) {
            free(data);
        }
    }
};

#define SEQ_MAX 128
deque<win_entry> window;
int window_size = 32;

#define BUFFER_SIZE 1024 * 512
char buffer[BUFFER_SIZE];

string file_path;

int sock;

struct sockaddr_in sin;

bool recv_data(int len) {
    if(!sanity_check(buffer, len)) return false;

    auto first = window.front();
    auto last = window.back();

    auto header = (packet_header *) buffer;
    to_host_format(header);

    if (header->seq_num >= first.seq_num && header->seq_num <= last.seq_num) {
        for(auto iter = window.begin(); iter != window.end(); iter++) {
            if((*iter).seq_num == header->seq_num) {
                //save the data if we hasn't received this packet before
                if((*iter).is_empty()) {
                    (*iter).set_data(buffer + sizeof(header), header->length - sizeof(header));
                }
                break;
            }
        }
    }
    if(!window.front().is_empty()) {
        //TODO we can now move the sliding window.
    }
    return false;
}

void try_receive() {
    int num_recv = 0;
    socklen_t len;
    if ((num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *) &sin, &len)) > 0) {
        recv_data(num_recv);
    }
}

int main() {

}