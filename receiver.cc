#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <deque>
#include <pthread.h>
#include <string>
#include <iostream>
#include <memory>

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
    void set_data(char *data_in, int dsize_in, int control_in) {
        control = control_in;
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

typedef shared_ptr<win_entry> win_entry_ptr;


int last_ack;
int last_control;


void save_file_data();

#define BATCH_SIZE 1024 * 512
struct file_holder {
    char buf[BATCH_SIZE];
    int buffer_size;
    int tot_bytes_saved;
    string subdir;
    string filename;
    int fd;
    ~file_holder() {
        save_file_data();
        close(fd);
    }
};

file_holder file;

bool init_file_holder() {
    mkdir(("recv-" + file.subdir).c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    file.fd = open(("recv-" + file.subdir + '/' + file.filename).c_str(), O_RDWR | O_FSYNC | O_CREAT, 0666);
    if (file.fd == -1) {
        return false;
    }
    ftruncate(file.fd, 0);
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

bool write_file_data(win_entry_ptr info) {
    if(info->is_empty()) return false;
    if(file.buffer_size + info->dsize > BATCH_SIZE) {
        save_file_data();
    }
    memcpy(file.buf + file.buffer_size, info->data, info->dsize);
    file.buffer_size += info->dsize;
    return true;
}


#define SEQ_MAX 128
deque<win_entry_ptr> window;
int window_size = 32;

#define BUFFER_SIZE 1024 * 512
char buffer[BUFFER_SIZE];

int sock;

struct sockaddr_in sin, sender_addr;

socklen_t sender_addr_len;

bool done = false;

bool send_data(unsigned int ack, unsigned int control) {
    auto header = (packet_header *) buffer;
    header->seq_num = ack;
    header->length = sizeof(packet_header);
    header->control = control;
    header->checksum = 0;
    header->checksum = gen_checksum(header, header->length);
    print_header(header);

    to_network_format(header);

    sendto(sock, buffer, sizeof(packet_header), 0, (struct sockaddr *) &sender_addr, sender_addr_len);
    cout << "ack sent: " << ack << endl;
}

bool recv_data(int len) {
    cout << "data received: " << len << " bytes" << endl;

    if(!sanity_check(buffer, len)) return false;

    cout << "ok" << endl;

    auto header = (packet_header *) buffer;

    if (header->seq_num >= window.front()->seq_num && header->seq_num <= window.back()->seq_num) {
        for(auto e : window) {
            if(e->seq_num == header->seq_num) {
                //save the data if we hasn't received this packet before
                if(e->is_empty()) {
                    e->set_data(buffer + sizeof(packet_header), header->length - sizeof(packet_header), header->control);
                }
                break;
            }
        }
    }
    if(!window.front()->is_empty()) {
        //we can now move the sliding window.
        int sz = 0;
        while(!window.empty() && !window.front()->is_empty()) {
            sz++;
            if(window.front()->control & CONTROL_META_DATA) {
                if(window.front()->seq_num == 0) {
                    file.subdir = string(window.front()->data, window.front()->dsize);
                }
                else {
                    file.filename = string(window.front()->data, window.front()->dsize);
                    init_file_holder();
                }
            }
            else {
                write_file_data(window.front());
            }
            last_ack = window.front()->seq_num;
            last_control = window.front()->control;
            window.pop_front();
        }
        while(sz--) {
            window.emplace_back(make_shared<win_entry>((window.back()->seq_num + 1) % SEQ_MAX));
        }
        //send the ack to the sever
    }
    send_data(last_ack, last_control);

    // if(last_ack == 18) {
    //     done = true;
    // }

    return true;
}

void try_receive() {
    int num_recv = 0;
    if ((num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *) &sender_addr, &sender_addr_len)) > 0) {
        recv_data(num_recv);
    }
}

int main(int argc, char **argv) {

    unsigned short receiver_port;
    int arg_check = 0;
    if(argc < 3) {
        fprintf(stderr, "too few arguments!\n");
        return -1;
    }

    for(int i = 1; i < argc; i++) {
        string cmd = string(argv[i]);
        if(cmd == "-p" && i + 1 < argc) {
            i++;
            arg_check++;
            receiver_port = atoi(argv[i]);;
        }
        else {
            fprintf(stderr, "unknown option!\n");
            return -1;
        }
    }

    if(arg_check != 1) {
        fprintf(stderr, "missing arguments!\n");
        return -1;
    }



    if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        fprintf(stderr, "opening socket");
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    memset(&sender_addr, 0, sizeof(sender_addr));

    sender_addr_len = sizeof(sender_addr);

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(receiver_port);

    cout << receiver_port << endl;
    /* bind socket to the address */
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "binding socket to address");
        return -1;
    }

    //init the receive window
    for(int i = 0; i < window_size; i++) {
        window.emplace_back(make_shared<win_entry>(i));
    }

    last_ack = SEQ_MAX - 1;
    last_control = 0;

    while(1 && !done) {
        try_receive();
    }

}