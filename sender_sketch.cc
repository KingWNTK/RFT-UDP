
typedef struct win_entry {
    int seq_num;
    int retrans_cd;
    char *s;
    int file_offset;
} win_entry;

typedef struct recv_packet {
    int ack;
    bool is_file_data;
} recv_packet;

typedef struct send_packet {

} send_packet;
#define SEQ_MAX 128
#define WIN_SIZE 64
deque<win_entry> window;
int window_sent;
int window_size;

int file_offset_max;

int retrans_cd_max;

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
                    window.push_back(win_entry{(last_seq_num + i) % SEQ_MAX, 0, NULL, i});
                }
            }
        } else {
            //sz packets are received, try to fill the window with sz more packets
            for (int i = 0; i < sz; i++) {
                window.pop_front();
            }
            for (int i = 0; i < sz; i++) {
                if (window.back()->offset < file_offset_max) {
                    window.push_back(win_entry{(last_seq_num + 1) % SEQ_MAX, 0, NULL, last->offset + 1});
                } else {
                    break;
                }
            }
        }
    }

    if(window.empty()) return true;
    return false;
}

void send_data(win_entry *info) {
    /** 
     * fill the buffer according to info
     */
    sendTo(buf);
    info->retrans_cd = retrans_cd_max;
}

void update_retrans_cd() {
    for(auto iter = window.begin(); iter != window.end(); iter++) {
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
    window.push_back({0, 0, "directory", 0});
    window.push_back({1, 0, "filename", 0});
    while (1) {
        pthread_mutex_lock(&mutex);
        try_receive();
        try_send();
        pthread_mutex_unlock(&mutex);
    }
}