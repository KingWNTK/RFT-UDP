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
#include <memory>

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

    ~file_holder() {
        close(fd);
    }
};

file_holder file;

#define SEQ_MAX 128
deque<win_entry_ptr> window;
int window_size = 32;

int retrans_cd_max = 10;

int packet_dsize_max = 1024 * 32;

#define BUFFER_SIZE 1024 * 512
char buffer[BUFFER_SIZE];

pthread_mutex_t mutex;

int sock;

struct sockaddr_in sin;

socklen_t sin_len;

bool done = false;


bool init_file_holder(string path) {
    file.fd = open(path.c_str(), O_RDONLY | O_FSYNC);
    if (file.fd == -1) {
        return false;
    }
    file.tot_bytes = lseek(file.fd, 0, SEEK_END);
    if (file.tot_bytes == -1) {
        return false;
    }
    if (lseek(file.fd, 0, SEEK_SET)) {
        return false;
    }
    file.tot_bytes_sent = file.buffer_sent = file.buffer_size = file.buffer_sent = 0;
    memset(file.buf, 0, sizeof(file.buf));
    return true;
}

bool refill_file_buf() {
    if (file.tot_bytes_sent == file.tot_bytes) {
        return false;
    }
    file.buffer_size = min(BATCH_SIZE, file.tot_bytes - file.tot_bytes_sent);
    //read min(BATCH_SIZE, tot_bytes - tot_bytes-snet) bytes starting from tot_bytes_sent from the fd
    lseek(file.fd, file.tot_bytes_sent, SEEK_SET);
    read(file.fd, file.buf, file.buffer_size);
    file.buffer_sent = 0;
    return true;
}

bool get_file_data(win_entry_ptr info) {
    cout << "trying to get file data" << endl;

    cout << file.buffer_sent << ' ' << file.buffer_size << endl;
    if (file.buffer_sent == file.buffer_size) {
        if (!refill_file_buf())
            return false;
    }
    pthread_mutex_lock(&mutex);
    info->dsize = min(info->dsize, file.buffer_size - file.buffer_sent);
    info->data = (char *) malloc(info->dsize);
    memcpy(info->data, file.buf + file.buffer_sent, info->dsize);
    file.tot_bytes_sent += info->dsize;
    file.buffer_sent += info->dsize;
    pthread_mutex_unlock(&mutex);
    cout << "get file data done" << endl;
 
    return true;
}

bool recv_data(int len) {
    cout << "data received: " << len << " bytes" << endl;

    if (!sanity_check(buffer, len))
        return false;

    auto header = (packet_header *) buffer;

    if (header->seq_num >= window.front()->seq_num && header->seq_num <= window.back()->seq_num) {
        int sz = (header->seq_num - window.front()->seq_num + 1 + SEQ_MAX) % SEQ_MAX;
        int last_seq_num = window.back()->seq_num;
        if (header->control & CONTROL_META_DATA) {
            if (header->seq_num == 1) {
                //if the filename and directory is received, init the window to send file buf
                window = deque<win_entry_ptr>{};
                for (int i = 1; i <= window_size; i++) {
                    window.emplace_back(make_shared<win_entry>((last_seq_num + i) % SEQ_MAX, nullptr, packet_dsize_max, 0));
                }

            }
        } else {
            //sz packets are received, try to fill the window with sz more packets
            for (int i = 0; i < sz; i++) {
                window.pop_front();
            }

            while (window.size() < window_size) {
                window.emplace_back(make_shared<win_entry>((last_seq_num + 1) % SEQ_MAX, nullptr, packet_dsize_max, 0));
            }
        }
    }

    // if(header->seq_num == 18) {
    //     done = true;
    // }

    return true;
}

bool send_data(win_entry_ptr info) {
    cout << "trying to send data" << endl;

    /** 
     * fill the buffer
     */
    if (!info->data) {
        if (!get_file_data(info)) {
            //the whole file is sent
            return false;
        }
    }
    cout << info->dsize << endl;
    auto header = (packet_header *) buffer;
    header->seq_num = info->seq_num;
    header->control = info->control;
    header->checksum = 0;
    header->length = sizeof(packet_header) + info->dsize;
    memcpy(buffer + sizeof(packet_header), info->data, info->dsize);
    cout << "ready" << endl;
    header->checksum = gen_checksum(buffer, header->length);

    to_network_format(header);

    sendto(sock, buffer, info->dsize + sizeof(packet_header), MSG_DONTWAIT, (struct sockaddr *) &sin, sizeof(sin));

    cout << "data sent, seq num: " << info->seq_num << ", times sent: " << info->times_sent << endl;

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
    if ((num_recv = recvfrom(sock, buffer, BUFFER_SIZE, MSG_DONTWAIT, (struct sockaddr *) &sin, &sin_len)) > 0) {
        cout << "received" << endl;
        pthread_mutex_lock(&mutex);
        if(recv_data(num_recv)) {
            
        }
        pthread_mutex_unlock(&mutex);
    }
}

void try_send() {
    for (auto e : window) {
        if (e->retrans_cd <= 0) {
            if(!send_data(e)) {
                // done = true;
            }
            break;
        }
    }
}

void *my_clock(void *arg) {
    while (1 && !done) {
        //sleep for 100 microseconds, can be modified
        usleep(100);
        pthread_mutex_lock(&mutex);
        update_retrans_cd();
        pthread_mutex_unlock(&mutex);
    }
    return nullptr;
}

void *my_send(void *arg) {
             //path
    window = {make_shared<win_entry>(0, ((string *)arg)[0].c_str(), CONTROL_META_DATA),
              //filename
              make_shared<win_entry>(1, ((string *) arg)[1].c_str(), CONTROL_META_DATA)};

    while (1 && !done) {
        try_receive();
        try_send();
    }
    return nullptr;
}

int main(int argc, char **argv) {
    string recv_host, recv_port, subdir, filename;

    if(argc < 4) {
        fprintf(stderr, "too few arguments!\n");
        return -1;
    }

    for(int i = 1; i < argc; i++) {
        string cmd = string(argv[i]);
        if(cmd == "-r") {
            i++;
            string tmp = string(argv[i]);
            int p = tmp.find(':');
            recv_host = tmp.substr(0, p);
            recv_port = tmp.substr(p + 1, tmp.length() - p);
        }
        else if(cmd == "-f") {
            i++;
            string tmp = string(argv[i]);
            int p = tmp.find('/');
            subdir = tmp.substr(0, p);
            filename = tmp.substr(p + 1, tmp.length() - p);
        }
        else {
            fprintf(stderr, "unknown option!\n");
            return -1;
        }
    }

    /**
     * init the file holder
     */
    if (!init_file_holder(subdir + '/' + filename)) {
        fprintf(stderr, "failed to open file");
        return -1;
    }
    refill_file_buf();

    /* variables for identifying the receiver */
    unsigned int receiver_addr;
    struct addrinfo *getaddrinfo_result, hints;

    /* convert server domain name to IP address */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET; /* indicates we want IPv4 */

    if (getaddrinfo(recv_host.c_str(), nullptr, &hints, &getaddrinfo_result) == 0) {
        receiver_addr = (unsigned int) ((struct sockaddr_in *) (getaddrinfo_result->ai_addr))->sin_addr.s_addr;
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
    string send_arg[2];
    send_arg[0] = subdir;
    send_arg[1] = filename;

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