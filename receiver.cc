#include <arpa/inet.h>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"

using namespace std;

struct win_entry {
    int seq_num;
    char *data;
    int dsize;
    int control;
    win_entry(int seq_num_in) : seq_num(seq_num_in), data(nullptr), dsize(0), control(0) {}
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

#define BATCH_SIZE 1024 * 512
struct file_holder {
    char buf[BATCH_SIZE];
    int buffer_size;
    int tot_bytes_saved;
    string subdir;
    string filename;
    int fd;
    bool init_file_holder() {
        mkdir(subdir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        fd = open((subdir + '/' + filename + ".recv").c_str(), O_RDWR | O_FSYNC | O_CREAT, 0666);
        if (fd == -1) {
            return false;
        }
        ftruncate(fd, 0);
        buffer_size = tot_bytes_saved = 0;
        memset(buf, 0, sizeof(buf));
        return true;
    }
    void save_file_data() {
        lseek(fd, 0, SEEK_END);
        write(fd, buf, buffer_size);
        tot_bytes_saved += buffer_size;
        buffer_size = 0;
    }
    bool write_file_data(win_entry_ptr info) {
        if (info->is_empty())
            return false;
        if (buffer_size + info->dsize > BATCH_SIZE) {
            save_file_data();
        }
        memcpy(buf + buffer_size, info->data, info->dsize);
        buffer_size += info->dsize;
        return true;
    }
    ~file_holder() {
        save_file_data();
        close(fd);
    }
};

file_holder file;

deque<win_entry_ptr> window;
int window_size = 32;

#define BUFFER_SIZE 1024 * 512
char buffer[BUFFER_SIZE];

int sock;

struct sockaddr_in sin, sender_addr;

socklen_t sender_addr_len;

bool should_close = false;

int fin_cd = 0;

int fine_times_tried = 0;

pthread_mutex_t mutex;

inline int dis(int a, int b) {
    return (b - a + SEQ_MAX) % SEQ_MAX;
}

bool send_data(unsigned int ack, unsigned int control) {
    auto header = (packet_header *)buffer;
    header->seq_num = ack;
    header->length = sizeof(packet_header);
    header->control = control;
    header->checksum = 0;
    header->checksum = gen_checksum(header, header->length);
    to_network_format(header);

    sendto(sock, buffer, sizeof(packet_header), 0, (struct sockaddr *)&sender_addr, sender_addr_len);
    cout << "[send data] ack: " << ack << " control: " << control << endl;
}

int recv_data(int len) {
    if (!sanity_check(buffer, len)) {
        cout << "[recv corrupt packet]" << endl;
        return -1;
    }

    auto header = (packet_header *)buffer;
    cout << "[recv data] (" << header->length << " bytes) control: " << header->control
         << " seq_num: " << header->seq_num << endl;

    cout << window.front()->seq_num << ' ' << window.back()->seq_num << endl;

    if (header->control == CONTROL_FIN) {
        return CONTROL_FIN;
    }

    if ((window.front()->seq_num <= header->seq_num && dis(window.front()->seq_num, header->seq_num) < window.size()) || (window.back()->seq_num >= header->seq_num && dis(header->seq_num, window.back()->seq_num) < window.size())) {
        for (auto e : window) {
            if (e->seq_num == header->seq_num) {
                //save the data if we hasn't received this packet before
                if (e->is_empty()) {
                    e->set_data(buffer + sizeof(packet_header), header->length - sizeof(packet_header), header->control);
                }
                break;
            }
        }
    }
    if (!window.front()->is_empty()) {
        //we can now move the sliding window.
        int sz = 0;
        int base = window.back()->seq_num;
        while (!window.empty() && !window.front()->is_empty()) {
            sz++;
            if (window.front()->control & CONTROL_META_DATA) {
                if (window.front()->seq_num == 0) {
                    file.subdir = string(window.front()->data, window.front()->dsize);
                } else {
                    file.filename = string(window.front()->data, window.front()->dsize);
                    file.init_file_holder();
                }
            } else {
                file.write_file_data(window.front());
            }
            last_ack = window.front()->seq_num;
            last_control = window.front()->control;
            window.pop_front();
        }
        for (int i = 1; i <= sz; i++) {
            window.push_back(make_shared<win_entry>((base + i) % SEQ_MAX));
        }
        //send the ack to the sever
    }
    send_data(last_ack, last_control);

    return 0;
}

bool try_receive() {
    int num_recv = 0;
    if ((num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *)&sender_addr, &sender_addr_len)) > 0) {
        if (recv_data(num_recv) > 0) {
            return true;
        }
    }
    return false;
}

int try_recv_fin() {
    int num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *)&sender_addr, &sender_addr_len);
    if (num_recv <= 0 || !sanity_check(buffer, num_recv))
        return 0;
    auto header = (packet_header *)buffer;
    cout << "[recv fin] " << header->control << endl;

    if (header->control & CONTROL_FIN_ACK)
        return CONTROL_FIN_ACK;
    else if (header->control & CONTROL_FIN)
        return CONTROL_FIN;
    return 0;
}

void try_send_fin(unsigned int control) {
    auto header = (packet_header *)buffer;
    header->control = control;
    header->length = sizeof(packet_header);
    header->checksum = 0;
    header->seq_num = 0;
    header->checksum = gen_checksum(buffer, header->length);
    to_network_format(header);
    sendto(sock, buffer, sizeof(packet_header), MSG_DONTWAIT, (struct sockaddr *)&sender_addr, sizeof(sender_addr));
    cout << "[send fin] " << header->control << endl;
}

void *my_clock(void *arg) {
    while (1 && !should_close) {
        usleep(CLOCK_TICK_MICROS);
        pthread_mutex_lock(&mutex);
        fin_cd--;
        pthread_mutex_unlock(&mutex);
    }
    return nullptr;
}

void *my_fin(void *arg) {
    while (1) {
        pthread_mutex_lock(&mutex);
        if (fin_cd <= 0) {
            try_send_fin(CONTROL_FIN);
            fin_cd = FIN_RETRANS_CD;
            fine_times_tried++;
        }
        pthread_mutex_unlock(&mutex);
        if (try_recv_fin() == CONTROL_FIN_ACK || fine_times_tried >= 100) {
            should_close = true;
            break;
        }
    }
}

int main(int argc, char **argv) {

    unsigned short receiver_port;
    int arg_check = 0;
    if (argc < 3) {
        fprintf(stderr, "too few arguments!\n");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        string cmd = string(argv[i]);
        if (cmd == "-p" && i + 1 < argc) {
            i++;
            arg_check++;
            receiver_port = atoi(argv[i]);
            ;
        } else {
            fprintf(stderr, "unknown option!\n");
            return -1;
        }
    }

    if (arg_check != 1) {
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
    for (int i = 0; i < window_size; i++) {
        window.emplace_back(make_shared<win_entry>(i));
    }

    last_ack = SEQ_MAX - 1;
    last_control = 0;

    while(recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *)&sender_addr, &sender_addr_len) > 0);

    while (1) {
        if (try_receive()) {
            break;
        }
    }

    if (pthread_mutex_init(&mutex, nullptr) != 0) {
        fprintf(stderr, "init mutex failed\n");
        return -1;
    }

    int error;
    pthread_t my_clock_tid, my_fin_tid;
    error = pthread_create(&my_clock_tid, nullptr, &my_clock, nullptr);
    if (error != 0) {
        fprintf(stderr, "create my_clock failed\n");
        return -1;
    }
    error = pthread_create(&my_fin_tid, nullptr, &my_fin, nullptr);
    if (error != 0) {
        fprintf(stderr, "create my_fin failed\n");
        return -1;
    }
    pthread_join(my_clock_tid, nullptr);
    pthread_join(my_fin_tid, nullptr);
    pthread_mutex_destroy(&mutex);
    return 0;
}