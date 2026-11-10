#include "device.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

#define AUTO_COOLDOWN 15     // phut toi thieu giua 2 lan tu dong tuoi/bon theo nguong
#define WATER_EFFECT 1.5     // moi lit nuoc lam tang do am 1.5%
#define FERT_EFFECT 0.5      // moi gam phan lam tang N, P, K 0.5 mg/kg
#define LOG_LINE 256
#define MAX_LOG_LINES 200

Environment g_env = { 55.0, 35.0, 30.0, 32.0 };
long g_sim_min = 0;
int g_start_tod = 0;

static const char *TYPE_NAMES[DEV_TYPE_COUNT] = { TYPE_SENSOR, TYPE_PUMP, TYPE_FERTILIZER, TYPE_LIGHT };

const char *dev_type_name(DevType t){
    return TYPE_NAMES[t];
}

int dev_type_parse(const char *s){
    for (int i = 0; i < DEV_TYPE_COUNT; i++)
        if (strcasecmp(s, TYPE_NAMES[i]) == 0) return i;
    if (strcasecmp(s, "FERT") == 0) return DEV_FERTILIZER;
    return -1;
}

/* ===================== Thoi gian mo phong ===================== */

static int sim_tod(void){
    return (int)((g_start_tod + g_sim_min) % 1440);
}

static int sim_day(void){
    return (int)((g_start_tod + g_sim_min) / 1440) + 1;
}

static void fmt_hm(int minutes, char *buf){
    sprintf(buf, "%02d:%02d", (minutes / 60) % 24, minutes % 60);
}

static int parse_hm(const char *s, int *out){
    int h, m, used = 0;
    if (sscanf(s, "%d:%d%n", &h, &m, &used) != 2 || s[used] != '\0') return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    *out = h * 60 + m;
    return 0;
}

/* ===================== Thong so ===================== */

static void set_default_params(Device *d){
    d->interval = 30;

    d->hmin = 30;
    d->hmax = 70;
    d->water_vol = 10;
    d->water_times[0] = 6 * 60;
    d->water_times[1] = 17 * 60;
    d->n_water_times = 2;

    d->nmin = 20;
    d->pmin = 15;
    d->kmin = 18;
    d->conc = 2;
    d->fert_vol = 5;

    d->watt = 100;
    d->light_on_at[0] = 18 * 60;
    d->light_off_at[0] = 22 * 60;
    d->n_light_slots = 1;
}

static int parse_num(const char *s, double *out, double lo, double hi){
    char *end;
    double x = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(x) || x < lo || x > hi) return -1;
    *out = x;
    return 0;
}

/* "06:00,17:30" hoac "none" */
static int parse_times(const char *val, int *arr, int *n){
    if (strcasecmp(val, "none") == 0) {
        *n = 0;
        return 0;
    }
    char tmp[256], *save;
    snprintf(tmp, sizeof(tmp), "%s", val);
    int cnt = 0;
    for (char *p = strtok_r(tmp, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        if (cnt >= MAX_SLOTS || parse_hm(p, &arr[cnt]) < 0) return -1;
        cnt++;
    }
    if (cnt == 0) return -1;
    *n = cnt;
    return 0;
}

/* "18:00-22:00,04:00-06:00" hoac "none" */
static int parse_slots(const char *val, int *on, int *off, int *n){
    if (strcasecmp(val, "none") == 0) {
        *n = 0;
        return 0;
    }
    char tmp[256], *save;
    snprintf(tmp, sizeof(tmp), "%s", val);
    int cnt = 0;
    for (char *p = strtok_r(tmp, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        char *dash = strchr(p, '-');
        if (cnt >= MAX_SLOTS || dash == NULL) return -1;
        *dash = '\0';
        if (parse_hm(p, &on[cnt]) < 0 || parse_hm(dash + 1, &off[cnt]) < 0 || on[cnt] == off[cnt]) return -1;
        cnt++;
    }
    if (cnt == 0) return -1;
    *n = cnt;
    return 0;
}

static void format_times(const int *arr, int n, char *buf){
    if (n == 0) {
        strcpy(buf, "none");
        return;
    }
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        char hm[8];
        fmt_hm(arr[i], hm);
        if (i > 0) strcat(buf, ",");
        strcat(buf, hm);
    }
}

static void format_slots(const int *on, const int *off, int n, char *buf){
    if (n == 0) {
        strcpy(buf, "none");
        return;
    }
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        char a[8], b[8];
        fmt_hm(on[i], a);
        fmt_hm(off[i], b);
        if (i > 0) strcat(buf, ",");
        strcat(buf, a);
        strcat(buf, "-");
        strcat(buf, b);
    }
}

/* Gan 1 thong so theo loai thiet bi. Tra ve 0 neu hop le */
static int apply_param(Device *d, const char *key, const char *val){
    double x;
    switch (d->type) {
    case DEV_SENSOR:
        if (strcmp(key, "interval") == 0) {
            if (parse_num(val, &x, 1, 1440) < 0) return -1;
            d->interval = (int)x;
            d->next_record = g_sim_min + d->interval;
            return 0;
        }
        break;
    case DEV_PUMP:
        if (strcmp(key, "hmin") == 0) return parse_num(val, &d->hmin, 0, 100);
        if (strcmp(key, "hmax") == 0) return parse_num(val, &d->hmax, 0, 100);
        if (strcmp(key, "vol") == 0) return parse_num(val, &d->water_vol, 0.1, 1000);
        if (strcmp(key, "times") == 0) return parse_times(val, d->water_times, &d->n_water_times);
        break;
    case DEV_FERTILIZER:
        if (strcmp(key, "nmin") == 0) return parse_num(val, &d->nmin, 0, 1000);
        if (strcmp(key, "pmin") == 0) return parse_num(val, &d->pmin, 0, 1000);
        if (strcmp(key, "kmin") == 0) return parse_num(val, &d->kmin, 0, 1000);
        if (strcmp(key, "conc") == 0) return parse_num(val, &d->conc, 0.1, 100);
        if (strcmp(key, "vol") == 0) return parse_num(val, &d->fert_vol, 0.1, 1000);
        break;
    case DEV_LIGHT:
        if (strcmp(key, "watt") == 0) return parse_num(val, &d->watt, 1, 10000);
        if (strcmp(key, "times") == 0) return parse_slots(val, d->light_on_at, d->light_off_at, &d->n_light_slots);
        break;
    default:
        break;
    }
    return -1;
}

static int check_params(const Device *d){
    if (d->type == DEV_PUMP && d->hmin >= d->hmax) return -1;
    return 0;
}

/* Dinh dang thong so dang "key=value key=value" (cung cu phap voi lenh SET) */
static void format_params(const Device *d, char *buf, size_t sz){
    char t[256];
    switch (d->type) {
    case DEV_SENSOR:
        snprintf(buf, sz, "interval=%d", d->interval);
        break;
    case DEV_PUMP:
        format_times(d->water_times, d->n_water_times, t);
        snprintf(buf, sz, "hmin=%g hmax=%g vol=%g times=%s", d->hmin, d->hmax, d->water_vol, t);
        break;
    case DEV_FERTILIZER:
        snprintf(buf, sz, "nmin=%g pmin=%g kmin=%g conc=%g vol=%g",
                 d->nmin, d->pmin, d->kmin, d->conc, d->fert_vol);
        break;
    case DEV_LIGHT:
        format_slots(d->light_on_at, d->light_off_at, d->n_light_slots, t);
        snprintf(buf, sz, "watt=%g times=%s", d->watt, t);
        break;
    default:
        buf[0] = '\0';
    }
}

/* ===================== Khoi tao, luu trang thai, log ===================== */

static void log_path(const Device *d, char *buf, size_t sz){
    snprintf(buf, sz, "logs/%s.log", d->id);
}

static void state_path(const Device *d, char *buf, size_t sz){
    snprintf(buf, sz, "state/%s.state", d->id);
}

void dev_init(Device *d, DevType type, const char *id, const char *password, int port){
    memset(d, 0, sizeof(*d));
    d->type = type;
    snprintf(d->id, sizeof(d->id), "%s", id);
    snprintf(d->password, sizeof(d->password), "%s", password);
    d->port = port;
    d->power = 1;
    d->listen_fd = -1;
    d->last_record_at = -1;
    d->last_auto = -AUTO_COOLDOWN;
    set_default_params(d);
    d->next_record = d->interval;

    char path[128];
    log_path(d, path, sizeof(path));
    d->logf = fopen(path, "a");
}

void dev_save_state(Device *d){
    char path[128], params[512];
    state_path(d, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f == NULL) return;
    format_params(d, params, sizeof(params));
    for (char *p = params; *p; p++)
        if (*p == ' ') *p = '\n';
    fprintf(f, "type=%s\npassword=%s\npower=%d\n%s\n",
            dev_type_name(d->type), d->password, d->power, params);
    fclose(f);
}

int dev_load_state(Device *d){
    char path[128], line[512];
    state_path(d, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        if (strcmp(key, "password") == 0) snprintf(d->password, sizeof(d->password), "%s", val);
        else if (strcmp(key, "power") == 0) d->power = atoi(val) ? 1 : 0;
        else if (strcmp(key, "type") != 0) apply_param(d, key, val);
    }
    fclose(f);
    if (check_params(d) < 0) set_default_params(d);
    d->next_record = g_sim_min + d->interval;
    return 0;
}

void dev_log(Device *d, const char *fmt, ...){
    char msg[512], hm[8];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fmt_hm(sim_tod(), hm);
    if (d->logf) {
        fprintf(d->logf, "[Ngay %d %s] %s\n", sim_day(), hm, msg);
        fflush(d->logf);
    }
    printf("[Ngay %d %s] %-10s %s\n", sim_day(), hm, d->id, msg);
    fflush(stdout);
}

/* ===================== Mo phong ===================== */

static double noise(double amp){
    return ((double)rand() / RAND_MAX * 2 - 1) * amp;
}

static double clamp(double x, double lo, double hi){
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Moi phut mo phong: dat mat nuoc va cay hap thu dinh duong */
void env_tick(void){
    g_env.humidity = clamp(g_env.humidity - 0.08 + noise(0.03), 0, 100);
    g_env.n = clamp(g_env.n - 0.03 + noise(0.01), 0, 1000);
    g_env.p = clamp(g_env.p - 0.02 + noise(0.01), 0, 1000);
    g_env.k = clamp(g_env.k - 0.025 + noise(0.01), 0, 1000);
}

static void take_reading(Device *d){
    d->last_reading = g_env;
    d->last_record_at = g_sim_min;
    dev_log(d, "Ghi nhan: do am=%.1f%% N=%.1f P=%.1f K=%.1f (mg/kg)",
            g_env.humidity, g_env.n, g_env.p, g_env.k);
}

static void do_water(Device *d, double liters, const char *reason){
    double before = g_env.humidity;
    g_env.humidity = clamp(g_env.humidity + liters * WATER_EFFECT, 0, 100);
    d->total_water += liters;
    dev_log(d, "Tuoi %.1f lit nuoc (%s). Do am %.1f%% -> %.1f%%", liters, reason, before, g_env.humidity);
}

static void do_fertilize(Device *d, double liters, const char *reason){
    double grams = d->conc * liters;
    g_env.n += grams * FERT_EFFECT;
    g_env.p += grams * FERT_EFFECT;
    g_env.k += grams * FERT_EFFECT;
    g_env.humidity = clamp(g_env.humidity + liters * WATER_EFFECT, 0, 100);
    d->total_fert += grams;
    dev_log(d, "Bon %.1f g phan NPK hoa vao %.1f lit nuoc (%s). N=%.1f P=%.1f K=%.1f",
            grams, liters, reason, g_env.n, g_env.p, g_env.k);
}

static int in_light_slot(const Device *d, int tod){
    for (int i = 0; i < d->n_light_slots; i++) {
        int on = d->light_on_at[i], off = d->light_off_at[i];
        if (on < off ? (tod >= on && tod < off) : (tod >= on || tod < off)) return 1;
    }
    return 0;
}

static void update_light(Device *d){
    int manual = g_sim_min < d->manual_until;
    int want = d->power && (manual || in_light_slot(d, sim_tod()));
    if (want != d->light_state) {
        d->light_state = want;
        if (want) dev_log(d, "Bat den %.0f W (%s)", d->watt, manual ? "thu cong" : "theo lich");
        else dev_log(d, "Tat den");
    }
}

/* Goi moi phut mo phong cho tung thiet bi */
void dev_tick(Device *d){
    int tod = sim_tod();
    char reason[128], hm[8];

    if (d->type == DEV_LIGHT) {
        update_light(d);
        if (d->light_state) d->energy_wh += d->watt / 60.0;
        return;
    }
    if (!d->power) return;

    switch (d->type) {
    case DEV_SENSOR:
        if (g_sim_min >= d->next_record) {
            take_reading(d);
            d->next_record = g_sim_min + d->interval;
        }
        break;
    case DEV_PUMP:
        // Co che 1: tuoi theo lich neu do am khong vuot Hmax
        for (int i = 0; i < d->n_water_times; i++) {
            if (d->water_times[i] != tod) continue;
            fmt_hm(tod, hm);
            if (g_env.humidity <= d->hmax) {
                snprintf(reason, sizeof(reason), "lich tuoi %s", hm);
                do_water(d, d->water_vol, reason);
            } else {
                dev_log(d, "Bo qua lich tuoi %s: do am %.1f%% vuot Hmax %g%%", hm, g_env.humidity, d->hmax);
            }
        }
        // Co che 2: tuoi khi do am duoi Hmin
        if (g_env.humidity < d->hmin && g_sim_min - d->last_auto >= AUTO_COOLDOWN) {
            snprintf(reason, sizeof(reason), "do am %.1f%% < Hmin %g%%", g_env.humidity, d->hmin);
            do_water(d, d->water_vol, reason);
            d->last_auto = g_sim_min;
        }
        break;
    case DEV_FERTILIZER:
        if ((g_env.n < d->nmin || g_env.p < d->pmin || g_env.k < d->kmin) &&
            g_sim_min - d->last_auto >= AUTO_COOLDOWN) {
            snprintf(reason, sizeof(reason), "dinh duong duoi nguong: N=%.1f/%g P=%.1f/%g K=%.1f/%g",
                     g_env.n, d->nmin, g_env.p, d->pmin, g_env.k, d->kmin);
            do_fertilize(d, d->fert_vol, reason);
            d->last_auto = g_sim_min;
        }
        break;
    default:
        break;
    }
}

/* ===================== Phien lam viec (token) ===================== */

static void random_token(char *out){
    unsigned char raw[TOKEN_LEN / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        for (size_t i = 0; i < sizeof(raw); i++) raw[i] = rand() & 0xFF;
    }
    if (fd >= 0) close(fd);
    for (size_t i = 0; i < sizeof(raw); i++) sprintf(out + 2 * i, "%02x", raw[i]);
}

static Session *find_session(Device *d, const char *token){
    if (token == NULL) return NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (d->sessions[i].token[0] && strcmp(d->sessions[i].token, token) == 0) return &d->sessions[i];
    return NULL;
}

static Session *new_session(Device *d, const char *app_id){
    Session *slot = NULL;
    // Moi app chi giu 1 token: ket noi lai thi cap token moi
    for (int i = 0; i < MAX_SESSIONS && !slot; i++)
        if (d->sessions[i].token[0] && strcmp(d->sessions[i].app_id, app_id) == 0) slot = &d->sessions[i];
    for (int i = 0; i < MAX_SESSIONS && !slot; i++)
        if (!d->sessions[i].token[0]) slot = &d->sessions[i];
    if (!slot) {
        slot = &d->sessions[0];
        for (int i = 1; i < MAX_SESSIONS; i++)
            if (d->sessions[i].created < slot->created) slot = &d->sessions[i];
    }
    random_token(slot->token);
    snprintf(slot->app_id, sizeof(slot->app_id), "%s", app_id);
    slot->created = g_sim_min;
    return slot;
}

/* ===================== Xu ly lenh ===================== */

static void send_info(Device *d, int fd){
    char buf[1024], params[512], hm[8];
    fmt_hm(sim_tod(), hm);
    format_params(d, params, sizeof(params));
    int len = snprintf(buf, sizeof(buf), "id=%s type=%s power=%s day=%d time=%s %s",
                       d->id, dev_type_name(d->type), d->power ? "ON" : "OFF", sim_day(), hm, params);

    switch (d->type) {
    case DEV_SENSOR:
        if (d->last_record_at < 0) {
            snprintf(buf + len, sizeof(buf) - len, " last_record=none");
        } else {
            long t = g_start_tod + d->last_record_at;
            fmt_hm((int)(t % 1440), hm);
            snprintf(buf + len, sizeof(buf) - len, " last_record=Ngay%ld-%s humidity=%.1f n=%.1f p=%.1f k=%.1f",
                     t / 1440 + 1, hm, d->last_reading.humidity, d->last_reading.n,
                     d->last_reading.p, d->last_reading.k);
        }
        break;
    case DEV_PUMP:
        snprintf(buf + len, sizeof(buf) - len, " humidity=%.1f total_water=%.1f", g_env.humidity, d->total_water);
        break;
    case DEV_FERTILIZER:
        snprintf(buf + len, sizeof(buf) - len, " n=%.1f p=%.1f k=%.1f total_fert=%.1f",
                 g_env.n, g_env.p, g_env.k, d->total_fert);
        break;
    case DEV_LIGHT:
        snprintf(buf + len, sizeof(buf) - len, " light=%s manual_min=%ld energy_wh=%.1f",
                 d->light_state ? "ON" : "OFF",
                 d->manual_until > g_sim_min ? d->manual_until - g_sim_min : 0, d->energy_wh);
        break;
    default:
        break;
    }
    send_linef(fd, "%d %s", RC_OK, buf);
}

static void send_log(Device *d, int fd, int n){
    static char ring[MAX_LOG_LINES][LOG_LINE];
    char path[128], line[LOG_LINE];
    log_path(d, path, sizeof(path));

    int count = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0') continue;
            strcpy(ring[count % n], line);
            count++;
        }
        fclose(f);
    }
    int k = count < n ? count : n;
    send_linef(fd, "%d %d", RC_LIST, k);
    for (int i = count - k; i < count; i++) send_linef(fd, "%s", ring[i % n]);
}

static void cmd_set(Device *d, int fd, char **save, const Session *s){
    Device tmp = *d;
    char applied[512] = "";
    int count = 0;
    // Kiem tra het tren ban sao truoc, chi ap dung khi tat ca hop le
    for (char *kv = strtok_r(NULL, " \t", save); kv; kv = strtok_r(NULL, " \t", save)) {
        char *eq = strchr(kv, '=');
        if (eq == NULL) {
            send_linef(fd, "%d Tham so khong hop le: %s", RC_BAD_REQUEST, kv);
            return;
        }
        *eq = '\0';
        if (apply_param(&tmp, kv, eq + 1) < 0) {
            send_linef(fd, "%d Tham so khong hop le: %s=%s", RC_BAD_REQUEST, kv, eq + 1);
            return;
        }
        *eq = '=';
        if (strlen(applied) + strlen(kv) + 2 < sizeof(applied)) {
            if (count > 0) strcat(applied, " ");
            strcat(applied, kv);
        }
        count++;
    }
    if (count == 0) {
        send_linef(fd, "%d Cu phap: SET <token> key=value ...", RC_BAD_REQUEST);
        return;
    }
    if (check_params(&tmp) < 0) {
        send_linef(fd, "%d Hmin phai nho hon Hmax", RC_BAD_REQUEST);
        return;
    }
    *d = tmp;
    dev_save_state(d);
    dev_log(d, "App %s cap nhat thong so: %s", s->app_id, applied);
    if (d->type == DEV_LIGHT) update_light(d);
    send_linef(fd, "%d Da cap nhat %d thong so", RC_OK, count);
}

static void cmd_run(Device *d, int fd, const char *arg, const Session *s){
    char reason[128];
    double x;
    if (!d->power) {
        send_linef(fd, "%d Thiet bi dang tat", RC_DEVICE_OFF);
        return;
    }
    snprintf(reason, sizeof(reason), "dieu khien truc tiep boi app %s", s->app_id);

    switch (d->type) {
    case DEV_SENSOR:
        take_reading(d);
        send_linef(fd, "%d Do am=%.1f%% N=%.1f P=%.1f K=%.1f", RC_OK,
                   d->last_reading.humidity, d->last_reading.n, d->last_reading.p, d->last_reading.k);
        break;
    case DEV_PUMP:
        x = d->water_vol;
        if (arg && parse_num(arg, &x, 0.1, 1000) < 0) {
            send_linef(fd, "%d So lit khong hop le (0.1 - 1000)", RC_BAD_REQUEST);
            return;
        }
        do_water(d, x, reason);
        send_linef(fd, "%d Da tuoi %.1f lit. Do am hien tai %.1f%%", RC_OK, x, g_env.humidity);
        break;
    case DEV_FERTILIZER:
        x = d->fert_vol;
        if (arg && parse_num(arg, &x, 0.1, 1000) < 0) {
            send_linef(fd, "%d So lit khong hop le (0.1 - 1000)", RC_BAD_REQUEST);
            return;
        }
        do_fertilize(d, x, reason);
        send_linef(fd, "%d Da bon %.1f g phan. N=%.1f P=%.1f K=%.1f", RC_OK,
                   d->conc * x, g_env.n, g_env.p, g_env.k);
        break;
    case DEV_LIGHT:
        if (arg == NULL || parse_num(arg, &x, 0, 1440) < 0) {
            send_linef(fd, "%d Cu phap: RUN <token> <so phut> (0 = huy bat thu cong)", RC_BAD_REQUEST);
            return;
        }
        d->manual_until = g_sim_min + (long)x;
        dev_log(d, "App %s bat den thu cong %ld phut", s->app_id, (long)x);
        update_light(d);
        send_linef(fd, "%d Den dang %s", RC_OK, d->light_state ? "BAT" : "TAT");
        break;
    default:
        break;
    }
}

int dev_handle_command(Device *d, int fd, char *line, const char *peer){
    char *save;
    char *cmd = strtok_r(line, " \t", &save);
    if (cmd == NULL) return 0;

    if (strcasecmp(cmd, "QUIT") == 0) {
        send_linef(fd, "%d Tam biet", RC_OK);
        return 1;
    }

    if (strcasecmp(cmd, "CONNECT") == 0) {
        char *app = strtok_r(NULL, " \t", &save);
        char *pass = strtok_r(NULL, " \t", &save);
        if (app == NULL || pass == NULL) {
            send_linef(fd, "%d Cu phap: CONNECT <app_id> <password>", RC_BAD_REQUEST);
        } else if (strcmp(pass, d->password) != 0) {
            dev_log(d, "App %s (%s) ket noi that bai: sai mat khau", app, peer);
            send_linef(fd, "%d Sai mat khau", RC_WRONG_PASSWORD);
        } else {
            Session *s = new_session(d, app);
            dev_log(d, "App %s (%s) ket noi thanh cong", app, peer);
            send_linef(fd, "%d %s %s %s", RC_OK, s->token, d->id, dev_type_name(d->type));
        }
        return 0;
    }

    static const char *AUTH_CMDS[] = { "CHPASS", "POWER", "INFO", "SET", "RESET", "RUN", "LOG" };
    int known = 0;
    for (size_t i = 0; i < sizeof(AUTH_CMDS) / sizeof(AUTH_CMDS[0]); i++)
        if (strcasecmp(cmd, AUTH_CMDS[i]) == 0) known = 1;
    if (!known) {
        send_linef(fd, "%d Lenh khong ho tro: %s", RC_UNKNOWN_CMD, cmd);
        return 0;
    }

    // Cac lenh con lai deu phai kem token hop le
    Session *s = find_session(d, strtok_r(NULL, " \t", &save));
    if (s == NULL) {
        send_linef(fd, "%d Token khong hop le, hay ket noi lai", RC_UNAUTHORIZED);
        return 0;
    }

    if (strcasecmp(cmd, "CHPASS") == 0) {
        char *oldp = strtok_r(NULL, " \t", &save);
        char *newp = strtok_r(NULL, " \t", &save);
        if (oldp == NULL || newp == NULL || strlen(newp) >= sizeof(d->password)) {
            send_linef(fd, "%d Cu phap: CHPASS <token> <mat khau cu> <mat khau moi>", RC_BAD_REQUEST);
        } else if (strcmp(oldp, d->password) != 0) {
            dev_log(d, "App %s doi mat khau that bai: sai mat khau cu", s->app_id);
            send_linef(fd, "%d Mat khau cu khong dung", RC_WRONG_PASSWORD);
        } else {
            strcpy(d->password, newp);
            // Huy token cua cac app khac: ho phai ket noi lai bang mat khau moi
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (&d->sessions[i] != s) d->sessions[i].token[0] = '\0';
            dev_save_state(d);
            dev_log(d, "App %s da doi mat khau", s->app_id);
            send_linef(fd, "%d Doi mat khau thanh cong", RC_OK);
        }
    } else if (strcasecmp(cmd, "POWER") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        if (arg == NULL || (strcasecmp(arg, "ON") != 0 && strcasecmp(arg, "OFF") != 0)) {
            send_linef(fd, "%d Cu phap: POWER <token> ON|OFF", RC_BAD_REQUEST);
        } else {
            d->power = strcasecmp(arg, "ON") == 0;
            if (d->type == DEV_SENSOR && d->power) d->next_record = g_sim_min;
            dev_save_state(d);
            dev_log(d, "App %s %s thiet bi", s->app_id, d->power ? "BAT" : "TAT");
            if (d->type == DEV_LIGHT) update_light(d);
            send_linef(fd, "%d Thiet bi da %s", RC_OK, d->power ? "BAT" : "TAT");
        }
    } else if (strcasecmp(cmd, "INFO") == 0) {
        send_info(d, fd);
    } else if (strcasecmp(cmd, "SET") == 0) {
        cmd_set(d, fd, &save, s);
    } else if (strcasecmp(cmd, "RESET") == 0) {
        set_default_params(d);
        d->next_record = g_sim_min + d->interval;
        dev_save_state(d);
        dev_log(d, "App %s khoi tao lai thong so mac dinh", s->app_id);
        if (d->type == DEV_LIGHT) update_light(d);
        send_linef(fd, "%d Da khoi tao thong so mac dinh", RC_OK);
    } else if (strcasecmp(cmd, "RUN") == 0) {
        cmd_run(d, fd, strtok_r(NULL, " \t", &save), s);
    } else if (strcasecmp(cmd, "LOG") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        int n = arg ? atoi(arg) : 20;
        if (n < 1) n = 1;
        if (n > MAX_LOG_LINES) n = MAX_LOG_LINES;
        send_log(d, fd, n);
    }
    return 0;
}
