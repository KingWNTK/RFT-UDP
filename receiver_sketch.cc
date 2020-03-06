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
    win_entry(int seq_num_in) : seq_num(seq_num_in), data(nullptr), dsize(0) {}
    bool is_empty() {
        return (!data);
    }
    set_data(char *data_in, int dsize_in, int control_in) : control(control_in), dsize(dsize_in) {
        data = (char *)malloc(dsize);
        memcpy(data, data_in, dsize);
    }
    ~win_entry() {
        if (data) {
            free(data);
        }
    }
};


#define BATCH_SIZE 1024 * 512
struct file_holder {
    char buf[BATCH_SIZE];
    int buffer_size;
    int tot_bytes_saved;
    string subdir;
    string filename;
    int fd;
    ~file_holder() {
        close(fd);
    }
};

file_holder file;

bool init_file_holder(string path) {
    file.fd = open(path.c_str(), O_RDONLY | O_FSYNC | O_CREAT);
    if (file.fd == -1) {
        return false;
    }
    file.buffer_size = file.tot_bytes_saved = 0;
    memset(file.buf, 0, sizeof(file.buf));
    return true;
}

void save_file_data() {
    lseek(file.fd, 0, SEEK_END);
    write(file.fd, file.buf, file.buffer_size);
    file.tot_bytes_saved += file.buffer_size;
    file.buffer_size = 0;
}

bool write_file_data(win_entry *info) {
    if(info->is_empty()) return false;
    if(file.buffer_size + info->dsize > BATCH_SIZE) {
        save_file_data();
    }
    memcpy(file.buf + file.buffer_size, info->data, info->dsize);
    file.buffer_size += info->dsize;
    return true;
}


#define SEQ_MAX 128
deque<win_entry> window;
int window_size = 32;

#define BUFFER_SIZE 1024 * 512
char buffer[BUFFER_SIZE];

string file_path;

int sock;

struct sockaddr_in sin;

bool send_data(int ack, int control) {
    auto header = (packet_header *) buffer;
    header->seq_num = ack;
    header->length = sizeof(header);
    header->control = control;
    header->checksum = 0;
    header->checksum = gen_checksum(header, header->length);
    to_network_format(header);

    sendto(sock, buffer, info->dsize + sizeof(packet_header), MSG_DONTWAIT, (struct sockaddr *) &sin, sizeof(sin));
}

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
                    (*iter).set_data(buffer + sizeof(header), header->length - sizeof(header), header->control);
                }
                break;
            }
        }
    }
    if(!window.front().is_empty()) {
        //we can now move the sliding window.
        int sz = 0;
        int ack;
        int control;
        while(!window.empty() && !window.front().is_empty()) {
            sz++;
            if(window.front().control & CONTROL_META_DATA) {
                auto front = window.front();
                if(front.seq_num == 0) {
                    file.subdir = string(front.data, front.dsize);
                }
                else {
                    file.filename = string(front.data, front.dsize);
                }
            }
            ack = window.front().seq_num;
            control = window.front().control;
            window.pop_front();
        }
        while(sz--) {
            window.push_back(win_entry((last.seq_num + 1) % SEQ_MAX));
        }
        //send the ack to the sever
        send_data(ack, control);
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