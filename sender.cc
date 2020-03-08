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
#include <sys/types.h>
#include <unistd.h>
#include <ctime>
#include <cstdlib>

#include "protocol.h"

using namespace std;

struct win_entry {
    int seq_num;
    int retrans_cd;
    int times_sent;
    char *data;
    int dsize;
    int control;
    win_entry(int seq_num_in, const char *data_in, int control_in) : seq_num(seq_num_in), control(control_in) {
        dsize = strlen(data_in);
        retrans_cd = times_sent = 0;
        data = (char *)malloc(dsize);
        memcpy(data, data_in, dsize);
    }
    win_entry(int seq_num_in, char *data_in, int dsize_in, int control_in) : seq_num(seq_num_in), data(data_in), dsize(dsize_in), control(control_in) {
        retrans_cd = times_sent = 0;
    }
    ~win_entry() {
        if (data) {
            free(data);
        }
    }
};

typedef shared_ptr<win_entry> win_entry_ptr;

#define BATCH_SIZE 1024 * 512

struct file_holder {
    char buf[BATCH_SIZE];
    int buffer_sent;
    int buffer_size;
    int tot_bytes_sent;
    int tot_bytes;
    int fd;

    bool init_file_holder(string path) {
        fd = open(path.c_str(), O_RDONLY | O_FSYNC);
        if (fd == -1) {
            return false;
        }
        tot_bytes = lseek(fd, 0, SEEK_END);
        if (tot_bytes == -1) {
            return false;
        }
        if (lseek(fd, 0, SEEK_SET)) {
            return false;
        }
        tot_bytes_sent = buffer_sent = buffer_size = buffer_sent = 0;
        memset(buf, 0, sizeof(buf));
        return true;
    }

    bool refill_file_buf() {
        if (tot_bytes_sent == tot_bytes) {
            return false;
        }
        buffer_size = min(BATCH_SIZE, tot_bytes - tot_bytes_sent);
        lseek(fd, tot_bytes_sent, SEEK_SET);
        read(fd, buf, buffer_size);
        buffer_sent = 0;
        return true;
    }

    bool get_file_data(win_entry_ptr info) {
        if (buffer_sent == buffer_size) {
            if (!refill_file_buf())
                return false;
        }
        info->dsize = min(info->dsize, buffer_size - buffer_sent);
        info->data = (char *)malloc(info->dsize);
        memcpy(info->data, buf + buffer_sent, info->dsize);
        tot_bytes_sent += info->dsize;
        buffer_sent += info->dsize;
        return true;
    }
    ~file_holder() {
        close(fd);
    }
};

file_holder file;

deque<win_entry_ptr> window;
int window_size = 32;

int retrans_cd_max = 10;

int last_seq_num = 0;


#define BUFFER_SIZE PACKET_SIZE_MAX
char buffer[BUFFER_SIZE];

pthread_mutex_t mutex;

int sock;

struct sockaddr_in sin;

socklen_t sin_len;

bool should_close = false;

int fin_cd = 0;

bool recv_data(int len) {
    if (!sanity_check(buffer, len)) {
        cout << "[recv corrupt packet]" << endl;
        return false;
    }

    if (window.empty()) {
        return true;
    }

    auto header = (packet_header *)buffer;
    cout << "[recv data] (" << header->length << " bytes) control: " << header->control
         << " seq_num: " << header->seq_num << endl;
    if (is_seq_in_window(header->seq_num, window.front()->seq_num, window.back()->seq_num, window.size())) {
    // if(((window.front()->seq_num <= header->seq_num && dis(window.front()->seq_num, header->seq_num) < window.size()) || (window.back()->seq_num >= header->seq_num && dis(header->seq_num, window.back()->seq_num) < window.size()))) {
        int sz = window.front()->seq_num <= header->seq_num ? dis(window.front()->seq_num, header->seq_num) + 1 : window.size() - dis(header->seq_num, window.back()->seq_num);
        if (header->control & CONTROL_FILEPATH) {
            //if the filename and directory is received, init the window
            window = deque<win_entry_ptr>{};
            for (int i = 1; i <= window_size; i++) {
                int nxt_seq = (last_seq_num + 1) % SEQ_MAX;
                auto e = make_shared<win_entry>(nxt_seq, nullptr, PACKET_DSIZE_MAX, 0);
                if (file.get_file_data(e)) {
                    last_seq_num = nxt_seq;
                    window.emplace_back(e);
                } else {
                    break;
                }
            }
        } else {
            //sz packets are received, try to fill the window with sz more packets
            for (int i = 0; i < sz; i++) {
                window.pop_front();
            }

            while (window.size() <= window_size) {
                int nxt_seq = (last_seq_num + 1) % SEQ_MAX;
                auto e = make_shared<win_entry>(nxt_seq, nullptr, PACKET_DSIZE_MAX, 0);
                if (file.get_file_data(e)) {
                    last_seq_num = nxt_seq;
                    window.emplace_back(e);
                } else {
                    break;
                }
            }
        }
    }

    return true;
}

bool send_data(win_entry_ptr info) {
    auto header = (packet_header *)buffer;
    header->seq_num = info->seq_num;
    header->control = info->control;
    header->checksum = 0;
    header->length = sizeof(packet_header) + info->dsize;
    memcpy(buffer + sizeof(packet_header), info->data, info->dsize);
    header->checksum = gen_checksum(buffer, header->length);

    to_network_format(header);

    sendto(sock, buffer, info->dsize + sizeof(packet_header), MSG_DONTWAIT, (struct sockaddr *)&sin, sizeof(sin));
    cout << "[send data] (" << info->dsize + sizeof(packet_header) << " bytes) seq num:"
         << info->seq_num << ", times sent: " << info->times_sent << endl;
    pthread_mutex_lock(&mutex);
    info->retrans_cd = retrans_cd_max;
    info->times_sent++;
    pthread_mutex_unlock(&mutex);
    return true;
}

void update_retrans_cd() {
    for (auto e : window) {
        e->retrans_cd--;
    }
}

void try_receive() {
    int num_recv = 0;
    if ((num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *)&sin, &sin_len)) > 0) {
        pthread_mutex_lock(&mutex);
        recv_data(num_recv);
        pthread_mutex_unlock(&mutex);
    }
}

void try_send() {
    for (auto e : window) {
        if (e->retrans_cd <= 0) {
            send_data(e);
            // if (e->times_sent > 50) {
            //     cout << "window size " << window.size() << endl;
            // }
            break;
        }
    }
}

int try_recv_fin() {
    int num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *)&sin, &sin_len);
    if (num_recv <= 0 || !sanity_check(buffer, num_recv))
        return 0;
    auto header = (packet_header *)buffer;
    if (header->seq_num != last_seq_num)
        return 0;
    cout << "[recv fin] " << header->control << ' ' << header->seq_num << endl;

    if (header->control & CONTROL_FIN)
        return CONTROL_FIN;
    if (header->control & CONTROL_FIN_ACK)
        return CONTROL_FIN_ACK;
    return 0;
}

void try_send_fin(unsigned int control, unsigned int seq_num) {
    auto header = (packet_header *)buffer;
    header->control = control;
    header->length = sizeof(packet_header);
    header->checksum = 0;
    header->seq_num = seq_num;
    header->checksum = gen_checksum(buffer, header->length);
    to_network_format(header);
    sendto(sock, buffer, sizeof(packet_header), MSG_DONTWAIT, (struct sockaddr *)&sin, sizeof(sin));
    cout << "[send fin] " << header->control << ' ' << seq_num << endl;
}

void *my_clock(void *arg) {
    while (1) {
        //sleep for 100 microseconds, can be modified
        usleep(CLOCK_TICK_MICROS);
        pthread_mutex_lock(&mutex);
        update_retrans_cd();
        pthread_mutex_unlock(&mutex);
        if (window.empty()) {
            break;
        }
    }
    while (1 && !should_close) {
        usleep(CLOCK_TICK_MICROS);
        pthread_mutex_lock(&mutex);
        fin_cd--;
        pthread_mutex_unlock(&mutex);
    }
    return nullptr;
}

void *my_send(void *arg) {
    //subdir and filename
    srand(time(NULL));
    last_seq_num = rand() % SEQ_MAX;
    window = {make_shared<win_entry>(last_seq_num, ((string *)arg)[0].c_str(), CONTROL_FILEPATH)};
    while (1) {
        try_receive();
        try_send();
        if (window.empty()) {
            break;
        }
    }
    while (1) {
        pthread_mutex_lock(&mutex);
        if (fin_cd <= 0) {
            fin_cd = FIN_RETRANS_CD;
            try_send_fin(CONTROL_FIN, last_seq_num);
        }
        pthread_mutex_unlock(&mutex);
        if (try_recv_fin()) {
            break;
        }
    }
    fin_cd = 10 * FIN_RETRANS_CD;
    while (1) {
        pthread_mutex_lock(&mutex);
        if (try_recv_fin()) {
            fin_cd = 10 * FIN_RETRANS_CD;
            try_send_fin(CONTROL_FIN_ACK, last_seq_num);
        }
        pthread_mutex_unlock(&mutex);

        if (fin_cd <= 0) {
            should_close = true;
            break;
        }
    }
    return nullptr;
}

int main(int argc, char **argv) {
    string recv_host, recv_port, subdir, filename, filepath;

    if (argc < 4) {
        fprintf(stderr, "too few arguments!\n");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        string cmd = string(argv[i]);
        if (cmd == "-r") {
            i++;
            string tmp = string(argv[i]);
            int p = tmp.find(':');
            recv_host = tmp.substr(0, p);
            recv_port = tmp.substr(p + 1, tmp.length() - p);
        } else if (cmd == "-f") {
            i++;
            string tmp = string(argv[i]);
            int p = tmp.find('/');
            subdir = tmp.substr(0, p);
            filename = tmp.substr(p + 1, tmp.length() - p);
            filepath = tmp;
            
        } else {
            fprintf(stderr, "unknown option!\n");
            return -1;
        }
    }

    /**
     * init the file holder
     */
    if (!file.init_file_holder(filepath)) {
        fprintf(stderr, "failed to open file");
        return -1;
    }
    file.refill_file_buf();

    /* variables for identifying the receiver */
    unsigned int receiver_addr;
    struct addrinfo *getaddrinfo_result, hints;

    /* convert server domain name to IP address */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET; /* indicates we want IPv4 */

    if (getaddrinfo(recv_host.c_str(), nullptr, &hints, &getaddrinfo_result) == 0) {
        receiver_addr = (unsigned int)((struct sockaddr_in *)(getaddrinfo_result->ai_addr))->sin_addr.s_addr;
        freeaddrinfo(getaddrinfo_result);
    } else {
        return -1;
    }
    /* create a socket */
    if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        fprintf(stderr, "opening UDP socket error");
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = receiver_addr;
    unsigned short server_port = atoi(recv_port.c_str());
    sin.sin_port = htons(server_port);

    if (pthread_mutex_init(&mutex, nullptr) != 0) {
        fprintf(stderr, "init mutex failed\n");
        return -1;
    }

    int error;
    pthread_t my_clock_tid, my_send_tid;
    error = pthread_create(&my_clock_tid, nullptr, &my_clock, nullptr);
    if (error != 0) {
        fprintf(stderr, "create my_clock failed\n");
        return -1;
    }
    string send_arg[1];
    send_arg[0] = filepath;

    error = pthread_create(&my_send_tid, nullptr, &my_send, (void *)send_arg);
    if (error != 0) {
        fprintf(stderr, "create my_send failed\n");
        return -1;
    }

    pthread_join(my_clock_tid, nullptr);
    pthread_join(my_send_tid, nullptr);
    pthread_mutex_destroy(&mutex);

    return 0;
}