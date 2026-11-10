#include "net.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void sb_init(StreamBuf *sb){
    sb->len = 0;
}

int sb_fill(StreamBuf *sb, int fd){
    if (sb->len >= SB_CAP) return -1;
    ssize_t n = recv(fd, sb->buf + sb->len, SB_CAP - sb->len, 0);
    if (n > 0) sb->len += n;
    return (int)n;
}

int sb_getline(StreamBuf *sb, char *out, size_t outsz){
    char *nl = memchr(sb->buf, '\n', sb->len);
    if (nl == NULL) {
        // Bo dem day ma khong co '\n' -> dong qua dai, bo toan bo
        if (sb->len >= SB_CAP) {
            sb->len = 0;
            return -1;
        }
        return 0;
    }
    size_t consumed = (size_t)(nl - sb->buf) + 1;
    size_t linelen = consumed - 1;
    if (linelen > 0 && sb->buf[linelen - 1] == '\r') linelen--;

    int rc = 1;
    if (linelen >= outsz) rc = -1;
    else {
        memcpy(out, sb->buf, linelen);
        out[linelen] = '\0';
    }
    memmove(sb->buf, sb->buf + consumed, sb->len - consumed);
    sb->len -= consumed;
    return rc;
}

int send_all(int fd, const void *data, size_t len){
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

int send_linef(int fd, const char *fmt, ...){
    char buf[SB_CAP];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return send_all(fd, buf, n);
}

int recv_line(int fd, StreamBuf *sb, char *out, size_t outsz, int timeout_ms){
    while (1) {
        int r = sb_getline(sb, out, outsz);
        if (r != 0) return r;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return -1;

        int n = sb_fill(sb, fd);
        if (n == 0) return 0;
        if (n < 0) return -1;
    }
}

int tcp_listen(int port){
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tcp_connect(const char *ip, int port, int timeout_ms){
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    // connect khong chan de co the gioi han thoi gian cho
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r < 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int err = 0;
        socklen_t elen = sizeof(err);
        if (poll(&pfd, 1, timeout_ms) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}
