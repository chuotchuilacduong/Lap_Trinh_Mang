/*
 * Chuong trinh dieu khien cho nguoi dung.
 *
 * - Moi ban cai dat co 1 App ID ngau nhien (luu trong <data>/app_id).
 * - Quan ly vuon cay, quet thiet bi bang UDP broadcast, ket noi bang mat khau
 *   de lay token, sau do moi lenh dieu khien deu gui kem token.
 * - Moi lan gui lenh mo 1 ket noi TCP toi thiet bi, gui lenh, doc phan hoi roi dong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"
#include "protocol.h"
#include "garden_store.h"

#define SCAN_TIMEOUT_MS 2000
#define REPLY_TIMEOUT_MS 5000
#define MAX_KV 32

static char g_app_id[32];
static char g_data_dir[200] = "data";
static char g_bcast[64] = "255.255.255.255";
static int g_scan_port = DEFAULT_SCAN_PORT;

/* ===================== Nhap lieu ===================== */

static int read_line(const char *prompt, char *buf, size_t sz){
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, sz, stdin) == NULL) {
        printf("\n");
        exit(0);
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    // bo khoang trang dau/cuoi
    char *p = buf + strspn(buf, " \t");
    memmove(buf, p, strlen(p) + 1);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
    return (int)n;
}

/* Tra ve def neu bo trong, -1 neu khong phai so */
static int read_int(const char *prompt, int def){
    char buf[64], *end;
    if (read_line(prompt, buf, sizeof(buf)) == 0) return def;
    long v = strtol(buf, &end, 10);
    return *end == '\0' ? (int)v : -1;
}

static void read_password(const char *prompt, char *buf, size_t sz){
    struct termios old, noecho;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        noecho = old;
        noecho.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
    }
    read_line(prompt, buf, sz);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        printf("\n");
    }
}

static int valid_word(const char *s){
    return s[0] != '\0' && strpbrk(s, " \t|") == NULL;
}

static void pause_enter(void){
    char buf[8];
    read_line("\n(Nhan Enter de tiep tuc)", buf, sizeof(buf));
}

/* ===================== Giao tiep voi thiet bi ===================== */

typedef struct {
    int fd;
    StreamBuf sb;
} Link;

static int link_open(CDevice *d, Link *l){
    l->fd = tcp_connect(d->ip, d->port, 3000);
    if (l->fd < 0) {
        printf("Khong ket noi duoc toi thiet bi %s (%s:%d)\n", d->id, d->ip, d->port);
        return -1;
    }
    sb_init(&l->sb);
    return 0;
}

static void link_close(Link *l){
    send_linef(l->fd, "QUIT");
    close(l->fd);
}

/* Gui 1 lenh, doc dong trang thai. Tra ve ma phan hoi, msg = phan noi dung sau ma */
static int link_request(Link *l, char *msg, size_t sz, const char *cmd){
    char line[MAX_LINE];
    if (send_linef(l->fd, "%s", cmd) < 0 || recv_line(l->fd, &l->sb, line, sizeof(line), REPLY_TIMEOUT_MS) != 1) {
        snprintf(msg, sz, "Mat ket noi hoac thiet bi khong phan hoi");
        return -1;
    }
    char *sp = strchr(line, ' ');
    snprintf(msg, sz, "%s", sp ? sp + 1 : "");
    return atoi(line);
}

static void handle_unauthorized(CDevice *d){
    d->token[0] = '\0';
    store_save();
    printf("Token khong con hieu luc (mat khau da doi hoac thiet bi khoi dong lai). Hay ket noi lai.\n");
}

/* Mo ket noi, gui 1 lenh, dong ket noi */
static int device_call(CDevice *d, char *msg, size_t sz, const char *fmt, ...){
    char cmd[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    Link l;
    if (link_open(d, &l) < 0) return -1;
    int code = link_request(&l, msg, sz, cmd);
    link_close(&l);
    if (code == RC_UNAUTHORIZED) handle_unauthorized(d);
    return code;
}

static void print_result(int code, const char *msg){
    if (code < 0) printf("Loi: %s\n", msg);
    else if (code == RC_OK) printf("Thanh cong: %s\n", msg);
    else if (code != RC_UNAUTHORIZED) printf("That bai (%d): %s\n", code, msg);
}

/* ===================== Phan tich INFO ===================== */

typedef struct {
    char key[32];
    char val[128];
} KV;

static int parse_kv(char *s, KV *kv, int max){
    int n = 0;
    char *save;
    for (char *tok = strtok_r(s, " ", &save); tok && n < max; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        snprintf(kv[n].key, sizeof(kv[n].key), "%s", tok);
        snprintf(kv[n].val, sizeof(kv[n].val), "%s", eq + 1);
        n++;
    }
    return n;
}

static const char *kv_get(const KV *kv, int n, const char *key){
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].key, key) == 0) return kv[i].val;
    return "?";
}

/* Ten hien thi cua cac khoa; type = NULL ap dung cho moi loai */
static const struct {
    const char *type, *key, *label;
} LABELS[] = {
    { NULL, "id", "ID thiet bi" },
    { NULL, "type", "Loai" },
    { NULL, "power", "Nguon" },
    { NULL, "day", "Ngay (mo phong)" },
    { NULL, "time", "Gio (mo phong)" },
    { NULL, "interval", "Chu ky ghi nhan T (phut)" },
    { NULL, "last_record", "Lan ghi nhan gan nhat" },
    { NULL, "humidity", "Do am dat (%)" },
    { NULL, "n", "Dam N (mg/kg)" },
    { NULL, "p", "Lan P (mg/kg)" },
    { NULL, "k", "Kali K (mg/kg)" },
    { NULL, "hmin", "Nguong do am toi thieu Hmin (%)" },
    { NULL, "hmax", "Nguong do am toi da Hmax (%)" },
    { TYPE_PUMP, "vol", "Luong nuoc moi lan tuoi V (lit)" },
    { TYPE_PUMP, "times", "Lich tuoi co dinh" },
    { NULL, "total_water", "Tong nuoc da tuoi (lit)" },
    { NULL, "nmin", "Nguong dam toi thieu Nmin" },
    { NULL, "pmin", "Nguong lan toi thieu Pmin" },
    { NULL, "kmin", "Nguong kali toi thieu Kmin" },
    { NULL, "conc", "Nong do phan bon C (g/lit)" },
    { TYPE_FERTILIZER, "vol", "Luong nuoc hoa phan V (lit)" },
    { NULL, "total_fert", "Tong phan da bon (g)" },
    { NULL, "watt", "Cong suat den P (W)" },
    { TYPE_LIGHT, "times", "Lich chieu sang" },
    { NULL, "light", "Trang thai den" },
    { NULL, "manual_min", "Bat thu cong con lai (phut)" },
    { NULL, "energy_wh", "Dien nang tieu thu (Wh)" },
};

static const char *label_of(const char *type, const char *key){
    for (size_t i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++)
        if (strcmp(LABELS[i].key, key) == 0 && (LABELS[i].type == NULL || strcmp(LABELS[i].type, type) == 0))
            return LABELS[i].label;
    return key;
}

/* Thong so co the thiet lap cho tung loai thiet bi */
static const struct {
    const char *type, *key, *hint;
} PARAMS[] = {
    { TYPE_SENSOR, "interval", "1-1440" },
    { TYPE_PUMP, "hmin", "0-100" },
    { TYPE_PUMP, "hmax", "0-100" },
    { TYPE_PUMP, "vol", "0.1-1000" },
    { TYPE_PUMP, "times", "HH:MM,HH:MM hoac none" },
    { TYPE_FERTILIZER, "nmin", ">= 0" },
    { TYPE_FERTILIZER, "pmin", ">= 0" },
    { TYPE_FERTILIZER, "kmin", ">= 0" },
    { TYPE_FERTILIZER, "conc", "0.1-100" },
    { TYPE_FERTILIZER, "vol", "0.1-1000" },
    { TYPE_LIGHT, "watt", "1-10000" },
    { TYPE_LIGHT, "times", "HH:MM-HH:MM,... hoac none" },
};

/* ===================== Chon thiet bi / vuon ===================== */

static void print_device_table(const char *garden){
    printf("%-4s %-12s %-12s %-11s %-22s %s\n", "STT", "Vuon", "ID", "Loai", "Dia chi", "Ket noi");
    for (int i = 0; i < g_ndevices; i++) {
        CDevice *d = &g_devices[i];
        if (garden && strcmp(d->garden, garden) != 0) continue;
        char addr[80];
        snprintf(addr, sizeof(addr), "%s:%d", d->ip, d->port);
        printf("%-4d %-12s %-12s %-11s %-22s %s\n", i + 1, d->garden, d->id, d->type, addr,
               d->token[0] ? "Da xac thuc" : "Chua");
    }
}

static CDevice *choose_device(void){
    if (g_ndevices == 0) {
        printf("Chua co thiet bi nao. Hay quet va them thiet bi vao vuon truoc.\n");
        return NULL;
    }
    print_device_table(NULL);
    int k = read_int("Chon thiet bi (0 = huy): ", 0);
    if (k < 1 || k > g_ndevices) return NULL;
    return &g_devices[k - 1];
}

static int do_connect(CDevice *d){
    char pass[64], msg[MAX_LINE];
    read_password("Mat khau thiet bi: ", pass, sizeof(pass));
    if (!valid_word(pass)) {
        printf("Mat khau khong hop le\n");
        return -1;
    }
    int code = device_call(d, msg, sizeof(msg), "CONNECT %s %s", g_app_id, pass);
    if (code != RC_OK) {
        print_result(code, msg);
        return -1;
    }
    char token[40], id[32], type[16];
    if (sscanf(msg, "%39s %31s %15s", token, id, type) != 3) {
        printf("Phan hoi khong hop le: %s\n", msg);
        return -1;
    }
    if (strcmp(id, d->id) != 0) {
        printf("Canh bao: dia chi nay hien la thiet bi %s, khong phai %s\n", id, d->id);
        return -1;
    }
    snprintf(d->token, sizeof(d->token), "%s", token);
    store_save();
    printf("Ket noi thanh cong toi %s (%s). Token: %s\n", d->id, type, token);
    return 0;
}

/* Chon thiet bi, neu chua co token thi ket noi luon */
static CDevice *choose_connected_device(void){
    CDevice *d = choose_device();
    if (d == NULL) return NULL;
    if (!d->token[0]) {
        printf("Thiet bi %s chua duoc xac thuc, tien hanh ket noi.\n", d->id);
        if (do_connect(d) < 0) return NULL;
    }
    return d;
}

static void print_garden_list(void){
    printf("%-4s %-16s %-10s %s\n", "STT", "Ten vuon", "Thiet bi", "Vi tri");
    for (int i = 0; i < g_ngardens; i++)
        printf("%-4d %-16s %-10d %s\n", i + 1, g_gardens[i].name,
               store_count_devices(g_gardens[i].name), g_gardens[i].location);
}

static Garden *create_garden(void){
    char name[32], loc[64];
    read_line("Ten vuon (khong dau cach): ", name, sizeof(name));
    if (!valid_word(name)) {
        printf("Ten vuon khong hop le\n");
        return NULL;
    }
    read_line("Vi tri / mo ta: ", loc, sizeof(loc));
    if (strchr(loc, '|')) {
        printf("Mo ta khong duoc chua ky tu '|'\n");
        return NULL;
    }
    Garden *g = store_add_garden(name, loc);
    if (g == NULL) printf("Vuon da ton tai hoac danh sach day\n");
    else {
        store_save();
        printf("Da tao vuon %s\n", name);
    }
    return g;
}

static Garden *choose_garden(int allow_create){
    if (g_ngardens > 0) print_garden_list();
    else printf("Chua co vuon nao.\n");
    int k = read_int(allow_create ? "Chon vuon (0 = tao vuon moi): " : "Chon vuon (0 = huy): ", 0);
    if (k == 0 && allow_create) return create_garden();
    if (k < 1 || k > g_ngardens) return NULL;
    return &g_gardens[k - 1];
}

/* ===================== Quan ly vuon ===================== */

static void show_garden(Garden *g){
    char msg[MAX_LINE];
    printf("\n=== Vuon %s (%s) ===\n", g->name, g->location);
    if (store_count_devices(g->name) == 0) {
        printf("Vuon chua co thiet bi.\n");
        return;
    }
    for (int i = 0; i < g_ndevices; i++) {
        CDevice *d = &g_devices[i];
        if (strcmp(d->garden, g->name) != 0) continue;
        printf("  - %-12s %-11s ", d->id, d->type);
        if (!d->token[0]) {
            printf("chua ket noi\n");
            continue;
        }
        fflush(stdout);
        int code = device_call(d, msg, sizeof(msg), "INFO %s", d->token);
        if (code != RC_OK) {
            if (code != RC_UNAUTHORIZED) printf("khong lay duoc thong tin\n");
            continue;
        }
        KV kv[MAX_KV];
        int n = parse_kv(msg, kv, MAX_KV);
        printf("nguon %-3s ", kv_get(kv, n, "power"));
        if (strcmp(d->type, TYPE_SENSOR) == 0)
            printf("do am %s%%, N/P/K %s/%s/%s (ghi luc %s)\n", kv_get(kv, n, "humidity"), kv_get(kv, n, "n"),
                   kv_get(kv, n, "p"), kv_get(kv, n, "k"), kv_get(kv, n, "last_record"));
        else if (strcmp(d->type, TYPE_PUMP) == 0)
            printf("do am %s%%, Hmin/Hmax %s/%s, da tuoi %s lit\n", kv_get(kv, n, "humidity"),
                   kv_get(kv, n, "hmin"), kv_get(kv, n, "hmax"), kv_get(kv, n, "total_water"));
        else if (strcmp(d->type, TYPE_FERTILIZER) == 0)
            printf("N/P/K %s/%s/%s, da bon %s g\n", kv_get(kv, n, "n"), kv_get(kv, n, "p"),
                   kv_get(kv, n, "k"), kv_get(kv, n, "total_fert"));
        else if (strcmp(d->type, TYPE_LIGHT) == 0)
            printf("den %s, %s W, lich %s\n", kv_get(kv, n, "light"), kv_get(kv, n, "watt"), kv_get(kv, n, "times"));
        else
            printf("\n");
    }
}

static void menu_gardens(void){
    while (1) {
        printf("\n--- QUAN LY VUON CAY ---\n"
               " 1. Danh sach vuon\n"
               " 2. Tao vuon moi\n"
               " 3. Xem chi tiet vuon\n"
               " 4. Chuyen thiet bi sang vuon khac\n"
               " 5. Xoa thiet bi khoi vuon\n"
               " 6. Xoa vuon\n"
               " 0. Quay lai\n");
        int c = read_int("Lua chon: ", -1);
        Garden *g;
        CDevice *d;
        char buf[8];
        switch (c) {
        case 0:
            return;
        case 1:
            if (g_ngardens == 0) printf("Chua co vuon nao.\n");
            else print_garden_list();
            break;
        case 2:
            create_garden();
            break;
        case 3:
            if ((g = choose_garden(0)) != NULL) show_garden(g);
            break;
        case 4:
            if ((d = choose_device()) == NULL) break;
            if ((g = choose_garden(1)) == NULL) break;
            snprintf(d->garden, sizeof(d->garden), "%s", g->name);
            store_save();
            printf("Da chuyen %s sang vuon %s\n", d->id, g->name);
            break;
        case 5:
            if ((d = choose_device()) == NULL) break;
            printf("Da xoa thiet bi %s khoi vuon %s\n", d->id, d->garden);
            store_remove_device(d);
            store_save();
            break;
        case 6:
            if ((g = choose_garden(0)) == NULL) break;
            printf("Xoa vuon %s cung %d thiet bi?", g->name, store_count_devices(g->name));
            read_line(" (y/n): ", buf, sizeof(buf));
            if (buf[0] == 'y' || buf[0] == 'Y') {
                char name[32];
                snprintf(name, sizeof(name), "%s", g->name);
                store_remove_garden(name);
                store_save();
                printf("Da xoa vuon %s\n", name);
            }
            break;
        default:
            printf("Lua chon khong hop le\n");
        }
    }
}

/* ===================== Quet thiet bi ===================== */

typedef struct {
    char id[32], type[16], ip[64], power[8];
    int port;
} Found;

static void menu_scan(void){
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        perror("socket");
        return;
    }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(g_scan_port);
    inet_pton(AF_INET, g_bcast, &dst.sin_addr);

    char msg[MAX_LINE];
    int len = snprintf(msg, sizeof(msg), "SCAN %s\r\n", g_app_id);
    if (sendto(s, msg, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("Gui yeu cau quet that bai");
        close(s);
        return;
    }
    printf("Dang quet thiet bi (%s:%d)...\n", g_bcast, g_scan_port);

    Found found[64];
    int nf = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = SCAN_TIMEOUT_MS - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) break;
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        if (poll(&pfd, 1, left) <= 0) break;

        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        msg[n] = '\0';

        Found f;
        if (sscanf(msg, "DEVICE %31s %15s %d %7s", f.id, f.type, &f.port, f.power) != 4) continue;
        inet_ntop(AF_INET, &from.sin_addr, f.ip, sizeof(f.ip));
        int dup = 0;
        for (int i = 0; i < nf; i++)
            if (strcmp(found[i].id, f.id) == 0) dup = 1;
        if (!dup && nf < 64) found[nf++] = f;
    }
    close(s);

    if (nf == 0) {
        printf("Khong tim thay thiet bi nao.\n");
        return;
    }
    printf("\nTim thay %d thiet bi:\n", nf);
    printf("%-4s %-12s %-11s %-22s %-6s %s\n", "STT", "ID", "Loai", "Dia chi", "Nguon", "Trong vuon");
    for (int i = 0; i < nf; i++) {
        char addr[80];
        snprintf(addr, sizeof(addr), "%s:%d", found[i].ip, found[i].port);
        CDevice *known = store_find_device(found[i].id);
        printf("%-4d %-12s %-11s %-22s %-6s %s\n", i + 1, found[i].id, found[i].type, addr, found[i].power,
               known ? known->garden : "(moi)");
        // Cap nhat dia chi neu thiet bi da biet doi IP/cong
        if (known && (strcmp(known->ip, found[i].ip) != 0 || known->port != found[i].port)) {
            store_add_device(known->garden, known->id, found[i].type, found[i].ip, found[i].port);
            store_save();
        }
    }

    while (1) {
        int k = read_int("\nThem thiet bi so may vao vuon (0 = xong): ", 0);
        if (k == 0) break;
        if (k < 1 || k > nf) {
            printf("So thu tu khong hop le\n");
            continue;
        }
        Garden *g = choose_garden(1);
        if (g == NULL) continue;
        Found *f = &found[k - 1];
        if (store_add_device(g->name, f->id, f->type, f->ip, f->port) == NULL) {
            printf("Danh sach thiet bi da day\n");
            continue;
        }
        store_save();
        printf("Da them %s (%s) vao vuon %s\n", f->id, f->type, g->name);
    }
}

/* ===================== Cac chuc nang dieu khien ===================== */

static void menu_connect(void){
    CDevice *d = choose_device();
    if (d) do_connect(d);
}

static void menu_change_password(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    char oldp[64], newp[64], again[64], msg[MAX_LINE];
    read_password("Mat khau cu: ", oldp, sizeof(oldp));
    read_password("Mat khau moi: ", newp, sizeof(newp));
    read_password("Nhap lai mat khau moi: ", again, sizeof(again));
    if (!valid_word(oldp) || !valid_word(newp)) {
        printf("Mat khau khong duoc rong hoac chua dau cach\n");
        return;
    }
    if (strcmp(newp, again) != 0) {
        printf("Mat khau nhap lai khong khop\n");
        return;
    }
    int code = device_call(d, msg, sizeof(msg), "CHPASS %s %s %s", d->token, oldp, newp);
    print_result(code, msg);
}

static void menu_power(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    int c = read_int("1. Bat   2. Tat   : ", 0);
    if (c != 1 && c != 2) return;
    char msg[MAX_LINE];
    int code = device_call(d, msg, sizeof(msg), "POWER %s %s", d->token, c == 1 ? "ON" : "OFF");
    print_result(code, msg);
}

static void menu_set_params(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    char msg[MAX_LINE];

    printf(" 1. Thiet lap thong so\n"
           " 2. Khoi tao lai thong so mac dinh\n");
    int c = read_int("Lua chon: ", 0);
    if (c == 2) {
        int code = device_call(d, msg, sizeof(msg), "RESET %s", d->token);
        print_result(code, msg);
        return;
    }
    if (c != 1) return;

    // Lay gia tri hien tai de nguoi dung biet
    int code = device_call(d, msg, sizeof(msg), "INFO %s", d->token);
    if (code != RC_OK) {
        print_result(code, msg);
        return;
    }
    KV kv[MAX_KV];
    int n = parse_kv(msg, kv, MAX_KV);

    printf("Nhap gia tri moi, bo trong de giu nguyen:\n");
    char cmd[MAX_LINE], val[128], prompt[256];
    int len = snprintf(cmd, sizeof(cmd), "SET %s", d->token), changed = 0;
    for (size_t i = 0; i < sizeof(PARAMS) / sizeof(PARAMS[0]); i++) {
        if (strcmp(PARAMS[i].type, d->type) != 0) continue;
        snprintf(prompt, sizeof(prompt), "  %s (%s) [%s]: ", label_of(d->type, PARAMS[i].key),
                 PARAMS[i].hint, kv_get(kv, n, PARAMS[i].key));
        if (read_line(prompt, val, sizeof(val)) == 0) continue;
        if (strpbrk(val, " \t")) {
            printf("  Gia tri khong duoc chua dau cach, bo qua\n");
            continue;
        }
        len += snprintf(cmd + len, sizeof(cmd) - len, " %s=%s", PARAMS[i].key, val);
        changed++;
    }
    if (changed == 0) {
        printf("Khong co thay doi\n");
        return;
    }
    code = device_call(d, msg, sizeof(msg), "%s", cmd);
    print_result(code, msg);
}

static void menu_direct_control(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    char arg[32] = "", msg[MAX_LINE];

    if (strcmp(d->type, TYPE_SENSOR) == 0) {
        printf("Yeu cau cam bien do ngay.\n");
    } else if (strcmp(d->type, TYPE_PUMP) == 0) {
        read_line("So lit nuoc can tuoi (Enter = luong mac dinh V): ", arg, sizeof(arg));
    } else if (strcmp(d->type, TYPE_FERTILIZER) == 0) {
        read_line("So lit dung dich can bon (Enter = luong mac dinh V): ", arg, sizeof(arg));
    } else if (strcmp(d->type, TYPE_LIGHT) == 0) {
        if (read_line("Bat den trong bao nhieu phut (0 = huy bat thu cong): ", arg, sizeof(arg)) == 0) return;
    }
    if (strpbrk(arg, " \t")) {
        printf("Gia tri khong hop le\n");
        return;
    }
    int code = arg[0] ? device_call(d, msg, sizeof(msg), "RUN %s %s", d->token, arg)
                      : device_call(d, msg, sizeof(msg), "RUN %s", d->token);
    print_result(code, msg);
}

static void menu_info(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    char msg[MAX_LINE];
    int code = device_call(d, msg, sizeof(msg), "INFO %s", d->token);
    if (code != RC_OK) {
        print_result(code, msg);
        return;
    }
    KV kv[MAX_KV];
    int n = parse_kv(msg, kv, MAX_KV);
    printf("\n--- Thong tin thiet bi %s (vuon %s) ---\n", d->id, d->garden);
    for (int i = 0; i < n; i++) printf("  %-34s %s\n", label_of(d->type, kv[i].key), kv[i].val);
}

static void menu_log(void){
    CDevice *d = choose_connected_device();
    if (d == NULL) return;
    int lines = read_int("So dong log can xem [20]: ", 20);
    if (lines < 1) lines = 20;

    Link l;
    char msg[MAX_LINE], cmd[64], line[MAX_LINE];
    if (link_open(d, &l) < 0) return;
    snprintf(cmd, sizeof(cmd), "LOG %s %d", d->token, lines);
    int code = link_request(&l, msg, sizeof(msg), cmd);
    if (code == RC_LIST) {
        int k = atoi(msg);
        printf("\n--- %d dong log gan nhat cua %s ---\n", k, d->id);
        // Phan hoi nhieu dong: doc tiep dung k dong tu cung luong
        for (int i = 0; i < k; i++) {
            if (recv_line(l.fd, &l.sb, line, sizeof(line), REPLY_TIMEOUT_MS) != 1) {
                printf("(mat ket noi khi dang doc log)\n");
                break;
            }
            printf("  %s\n", line);
        }
    }
    link_close(&l);
    if (code == RC_UNAUTHORIZED) handle_unauthorized(d);
    else if (code != RC_LIST) print_result(code, msg);
}

/* ===================== Khoi dong ===================== */

static void load_app_id(void){
    char path[256];
    snprintf(path, sizeof(path), "%s/app_id", g_data_dir);
    FILE *f = fopen(path, "r");
    if (f && fscanf(f, "%31s", g_app_id) == 1) {
        fclose(f);
        return;
    }
    if (f) fclose(f);

    // Lan dau cai dat: sinh App ID ngau nhien
    unsigned char raw[4];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw))
        for (size_t i = 0; i < sizeof(raw); i++) raw[i] = rand() & 0xFF;
    if (fd >= 0) close(fd);
    snprintf(g_app_id, sizeof(g_app_id), "APP-%02X%02X%02X%02X", raw[0], raw[1], raw[2], raw[3]);

    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", g_app_id);
        fclose(f);
    }
}

int main(int argc, char *argv[]){
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-b") == 0) snprintf(g_bcast, sizeof(g_bcast), "%s", argv[i + 1]);
        else if (strcmp(argv[i], "-p") == 0) g_scan_port = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-d") == 0) snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[i + 1]);
        else {
            fprintf(stderr, "Cach dung: %s [-b dia_chi_broadcast] [-p cong_quet] [-d thu_muc_du_lieu]\n", argv[0]);
            return 1;
        }
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    mkdir(g_data_dir, 0755);
    load_app_id();

    char path[256];
    snprintf(path, sizeof(path), "%s/gardens.txt", g_data_dir);
    store_load(path);

    while (1) {
        printf("\n===== DIEU KHIEN VUON CAY TU DONG  (App ID: %s) =====\n"
               " 1. Quan ly vuon cay\n"
               " 2. Quet thiet bi\n"
               " 3. Ket noi thiet bi\n"
               " 4. Doi mat khau thiet bi\n"
               " 5. Bat / tat thiet bi\n"
               " 6. Thiet lap thong so\n"
               " 7. Dieu khien truc tiep (bom nuoc, bon phan, bat den)\n"
               " 8. Xem thong tin thiet bi\n"
               " 9. Xem log hoat dong thiet bi\n"
               " 0. Thoat\n", g_app_id);
        int c = read_int("Lua chon: ", -1);
        switch (c) {
        case 0: return 0;
        case 1: menu_gardens(); continue;
        case 2: menu_scan(); break;
        case 3: menu_connect(); break;
        case 4: menu_change_password(); break;
        case 5: menu_power(); break;
        case 6: menu_set_params(); break;
        case 7: menu_direct_control(); break;
        case 8: menu_info(); break;
        case 9: menu_log(); break;
        default: printf("Lua chon khong hop le\n"); continue;
        }
        pause_enter();
    }
}
