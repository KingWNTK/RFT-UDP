
struct win_entry {
    int seq_num;
    int retrans_cd;
    int times_sent;
    char *data;
    int dsize;
    ~win_entry() {
        if (data) {
            free(data);
        }
    }
};

struct recv_packet {
    int ack;
    bool is_file_data;
};

struct send_packet {
};

#define BATCH_SIZE 1024 * 512
struct file_holder {
    char data[BATCH_SIZE];
    int offset_sent;
    int buffer_size;
    int tot_bytes_sent;
    int tot_bytes;
    int fd;
};

file_holder file;

#define SEQ_MAX 128
#define WIN_SIZE 64
deque<win_entry> window;
int window_sent;
int window_size;

int retrans_cd_max;

int packet_dsize_max;

char buf[BATCH_SIZE];

bool refill_file_data() {
    if (file.tot_bytes_sent == file.tot_bytes) {
        return false;
    }
    file.buffer_size = min(BATCH_SIZE, file.tot_bytes - file.tot_bytes_sent);
    //read min(BATCH_SIZE, tot_bytes - tot_bytes-snet) bytes starting from tot_bytes_sent from the fd
    file.offset_sent = 0;
    return true;
}

bool get_file_data(win_entry *info) {
    if (file.offset_sent == file.buffer_size) {
        if (!refill_file_data())
            return false;
    }
    info->dsize = min(info->dsize, file.buffer_size - file.offset_sent);
    info->data = malloc(info->dsize);
    memcpy(info->data, file.data + file.offset_sent, info->dsize);
    file.tot_bytes_sent += info->dsize;
    file.offset_sent += info->dsize;
    return true;
}

bool recv_data(char *buf) {
    if (!sanity_check(buf))
        return;
    recv_packet *packet = (recv_packet *)buf;

    if (window.empty())
        return true;

    auto first = window.front();
    auto last = window.back();

    if (packet->ack >= first.seq_num && packet->ack <= last.seq_num) {
        int sz = (packet->ack - first.seq_num + 1 + SEQ_MAX) % SEQ_MAX;
        int last_seq_num = last.seq_num;
        if (packet->is_file_data) {
            if (packet->ack == 1) {
                //if the filename and directory is received, init the window to send file data
                window.clear();
                for (int i = 1; i <= min(WIN_SIZE, file_offset_max); i++) {
                    window.push_back(win_entry{(last_seq_num + i) % SEQ_MAX, 0, 0, NULL, packet_dsize_max});
                }
            }
        } else {
            //sz packets are received, try to fill the window with sz more packets
            for (int i = 0; i < sz; i++) {
                window.pop_front();
            }
            for (int i = 0; i < sz; i++) {
                if (window.back()->offset < file_offset_max) {
                    window.push_back(win_entry{(last_seq_num + 1) % SEQ_MAX, 0, 0, NULL, packet_dsize_max});
                } else {
                    break;
                }
            }
        }
    }

    if (window.empty())
        return true;
    return false;
}

bool send_data(win_entry *info) {
    /** 
     * fill the buffer
     */
    if (info->times_sent == 0) {
        if (!get_file_data(info)) {
            //the whole file is sent
            return false;
        }
    }
    memcpy(buf, info->data, info->dsize);
    sendTo(buf, info->dsize);
    info->retrans_cd = retrans_cd_max;
    info->times_sent++;
    return true;
}

void update_retrans_cd() {
    for (auto iter = window.begin(); iter != window.end(); iter++) {
        iter->retrans_cd--;
    }
}

void try_receive() {
    if (recvFrom(buf) > 0) {
        recv_data(buf);
    }
}

void try_send() {
    for (auto iter = window.begin(); iter != window.end(); iter++) {
        if ((*iter).retrans_cd <= 0) {
            send_data(&(*iter));
            break;
        }
    }
}

void clock() {
    while (1) {
        sleep("1ms");
        pthread_mutex_lock(&mutex);
        update_retrans_cd();
        pthread_mutex_unlock(&mutex);
    }
}

int main() {
    window.push_back({0, 0, 0, "directory", 0});
    window.push_back({1, 0, 0, "filename", 0});
    while (1) {
        pthread_mutex_lock(&mutex);
        try_receive();
        try_send();
        pthread_mutex_unlock(&mutex);
    }
}