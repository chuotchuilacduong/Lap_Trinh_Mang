#ifndef NET_H
#define NET_H

#include <stddef.h>

/*
 * Bo dem dong (stream buffer): TCP la luong byte, mot lan recv() co the
 * nhan duoc nua thong diep hoac nhieu thong diep dinh lien nhau.
 * StreamBuf gom du lieu lai va tach ra tung dong hoan chinh theo '\n'.
 */
#define SB_CAP 4096

typedef struct {
    char buf[SB_CAP];
    size_t len;
} StreamBuf;

void sb_init(StreamBuf *sb);
/* Doc them du lieu tu socket vao bo dem. Tra ve so byte, 0 neu dong ket noi, <0 neu loi */
int sb_fill(StreamBuf *sb, int fd);
/* Lay 1 dong (bo "\r\n"). Tra ve 1 neu co dong, 0 neu chua du, -1 neu dong qua dai (bi bo) */
int sb_getline(StreamBuf *sb, char *out, size_t outsz);

int send_all(int fd, const void *data, size_t len);
/* Gui 1 dong theo dinh dang printf, tu dong them "\r\n" */
int send_linef(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Doc 1 dong (blocking, co timeout). Tra ve 1 neu co dong, 0 neu dong ket noi, -1 neu loi/timeout */
int recv_line(int fd, StreamBuf *sb, char *out, size_t outsz, int timeout_ms);

int tcp_listen(int port);
int tcp_connect(const char *ip, int port, int timeout_ms);

#endif
