/*
 * Chuong trinh mo phong cac cam bien va thiet bi trong 1 vuon cay.
 *
 * - Moi thiet bi co ID duy nhat, mat khau va cong TCP rieng.
 * - Tat ca thiet bi cung nghe thong diep quet (UDP) tren cong scan_port.
 * - Vao/ra socket dung poll(): 1 tien trinh, 1 luong phuc vu dong thoi
 *   socket UDP, cac socket lang nghe va moi ket noi cua ung dung dieu khien.
 * - Dong ho mo phong: moi giay thuc troi qua `speed` phut mo phong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"
#include "device.h"

#define MAX_DEVICES 16
#define MAX_CONNS 64

typedef struct {
    int fd;
    int dev;              // chi so thiet bi ma ket noi nay di vao
    char peer[32];
    StreamBuf sb;
} Conn;

static Device devices[MAX_DEVICES];
static int n_devices = 0;
static Conn conns[MAX_CONNS];
static int scan_port = DEFAULT_SCAN_PORT;
static int speed = 10;
static volatile sig_atomic_t running = 1;

static void on_signal(int sig){
    (void)sig;
    running = 0;
}

static int load_config(const char *path){
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    char line[256], type[32], id[32], pass[64];
    int port;
    while (fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (sscanf(p, "speed %d", &speed) == 1) continue;
        if (sscanf(p, "scan_port %d", &scan_port) == 1) continue;
        if (sscanf(p, "%31s %31s %63s %d", type, id, pass, &port) != 4) {
            fprintf(stderr, "Bo qua dong cau hinh: %s", p);
            continue;
        }
        int t = dev_type_parse(type);
        if (t < 0 || n_devices >= MAX_DEVICES) {
            fprintf(stderr, "Loai thiet bi khong hop le hoac qua nhieu thiet bi: %s\n", type);
            continue;
        }
        for (int i = 0; i < n_devices; i++) {
            if (strcmp(devices[i].id, id) == 0) {
                fprintf(stderr, "Trung ID thiet bi: %s\n", id);
                fclose(f);
                return -1;
            }
        }
        dev_init(&devices[n_devices++], t, id, pass, port);
    }
    fclose(f);
    return n_devices > 0 ? 0 : -1;
}

static int open_udp(int port){
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    // Cho phep nhieu tien trinh mo phong cung nghe 1 cong quet
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_scan(int udp){
    char buf[MAX_LINE + 1], reply[256];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(udp, buf, MAX_LINE, 0, (struct sockaddr *)&from, &flen);
    if (n <= 0) return;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    char app[64];
    if (sscanf(buf, "SCAN %63s", app) != 1) return;
    printf("Nhan yeu cau quet tu app %s (%s:%d)\n", app, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    for (int i = 0; i < n_devices; i++) {
        Device *d = &devices[i];
        int len = snprintf(reply, sizeof(reply), "DEVICE %s %s %d %s\r\n",
                           d->id, dev_type_name(d->type), d->port, d->power ? "ON" : "OFF");
        sendto(udp, reply, len, 0, (struct sockaddr *)&from, flen);
    }
}

static void accept_conn(Device *d){
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(d->listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) return;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].fd < 0) {
            conns[i].fd = fd;
            conns[i].dev = (int)(d - devices);
            snprintf(conns[i].peer, sizeof(conns[i].peer), "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
            sb_init(&conns[i].sb);
            return;
        }
    }
    send_linef(fd, "%d Thiet bi qua tai, thu lai sau", RC_SERVER_ERROR);
    close(fd);
}

static void close_conn(Conn *c){
    close(c->fd);
    c->fd = -1;
}

/* Doc du lieu moi, tach va xu ly tung dong lenh hoan chinh trong bo dem */
static void handle_conn(Conn *c){
    char line[MAX_LINE];
    int n = sb_fill(&c->sb, c->fd);
    if (n <= 0) {
        close_conn(c);
        return;
    }
    int r;
    while ((r = sb_getline(&c->sb, line, sizeof(line))) != 0) {
        if (r < 0) {
            send_linef(c->fd, "%d Thong diep qua dai", RC_BAD_REQUEST);
            continue;
        }
        if (dev_handle_command(&devices[c->dev], c->fd, line, c->peer)) {
            close_conn(c);
            return;
        }
    }
}

static long elapsed_ms(const struct timespec *start){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        fprintf(stderr, "Cach dung: %s <file_cau_hinh> [-s phut_mo_phong_moi_giay] [-p cong_quet]\n", argv[0]);
        return 1;
    }
    mkdir("state", 0755);
    mkdir("logs", 0755);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    g_start_tod = lt->tm_hour * 60 + lt->tm_min;

    if (load_config(argv[1]) < 0) {
        fprintf(stderr, "File cau hinh khong hop le\n");
        return 1;
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-s") == 0) speed = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-p") == 0) scan_port = atoi(argv[i + 1]);
    }
    if (speed < 1) speed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int udp = open_udp(scan_port);
    if (udp < 0) {
        perror("Khong mo duoc cong quet UDP");
        return 1;
    }
    for (int i = 0; i < n_devices; i++) {
        Device *d = &devices[i];
        // Khoi tao thong so: lay tu file trang thai neu da co, nguoc lai dung mac dinh
        if (dev_load_state(d) < 0) dev_save_state(d);
        d->listen_fd = tcp_listen(d->port);
        if (d->listen_fd < 0) {
            fprintf(stderr, "Khong mo duoc cong TCP %d cho thiet bi %s\n", d->port, d->id);
            return 1;
        }
        printf("Thiet bi %-10s loai %-10s cong TCP %d (%s)\n",
               d->id, dev_type_name(d->type), d->port, d->power ? "BAT" : "TAT");
        dev_log(d, "Khoi dong thiet bi");
    }
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
    printf("Nghe quet tren UDP %d, toc do mo phong %d phut/giay. Ctrl+C de dung.\n", scan_port, speed);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    struct pollfd pfds[1 + MAX_DEVICES + MAX_CONNS];
    int owner[1 + MAX_DEVICES + MAX_CONNS];   // vi tri trong conns[] cua tung pollfd ket noi

    while (running) {
        int nfds = 0;
        pfds[nfds].fd = udp;
        pfds[nfds++].events = POLLIN;
        for (int i = 0; i < n_devices; i++) {
            pfds[nfds].fd = devices[i].listen_fd;
            pfds[nfds++].events = POLLIN;
        }
        int first_conn = nfds;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd < 0) continue;
            owner[nfds] = i;
            pfds[nfds].fd = conns[i].fd;
            pfds[nfds++].events = POLLIN;
        }

        int ready = poll(pfds, nfds, 200);
        if (ready < 0 && errno != EINTR) {
            perror("poll");
            break;
        }

        // Tien dong ho mo phong theo thoi gian thuc
        long target = elapsed_ms(&start) * speed / 1000;
        while (g_sim_min < target) {
            g_sim_min++;
            env_tick();
            for (int i = 0; i < n_devices; i++) dev_tick(&devices[i]);
        }
        if (ready <= 0) continue;

        if (pfds[0].revents & POLLIN) handle_scan(udp);
        for (int i = 0; i < n_devices; i++)
            if (pfds[1 + i].revents & POLLIN) accept_conn(&devices[i]);
        for (int i = first_conn; i < nfds; i++)
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) handle_conn(&conns[owner[i]]);
    }

    printf("\nDang dung mo phong...\n");
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd >= 0) close(conns[i].fd);
    for (int i = 0; i < n_devices; i++) {
        dev_log(&devices[i], "Tat chuong trinh mo phong");
        dev_save_state(&devices[i]);
        close(devices[i].listen_fd);
        if (devices[i].logf) fclose(devices[i].logf);
    }
    close(udp);
    return 0;
}
