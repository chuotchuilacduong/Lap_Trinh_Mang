#ifndef DEVICE_H
#define DEVICE_H

#include <stdio.h>
#include "protocol.h"

#define MAX_SESSIONS 16
#define MAX_SLOTS 8

typedef enum { DEV_SENSOR, DEV_PUMP, DEV_FERTILIZER, DEV_LIGHT, DEV_TYPE_COUNT } DevType;

/* Trang thai dat trong vuon (dung chung cho moi thiet bi trong 1 lan mo phong) */
typedef struct {
    double humidity;   // do am (%)
    double n, p, k;    // nong do dam, lan, kali (mg/kg)
} Environment;

/* Moi ung dung da xac thuc duoc cap 1 token ngau nhien gan voi ID cua ung dung */
typedef struct {
    char token[TOKEN_LEN + 1];
    char app_id[64];
    long created;      // phut mo phong luc tao, de thay the session cu nhat khi day
} Session;

typedef struct {
    DevType type;
    char id[32];
    char password[64];
    int port;
    int power;                      // 1 = bat, 0 = tat
    int listen_fd;
    FILE *logf;
    Session sessions[MAX_SESSIONS];

    /* Cam bien moi truong */
    int interval;                   // T: chu ky ghi nhan (phut)
    long next_record;
    long last_record_at;            // -1 neu chua ghi lan nao
    Environment last_reading;

    /* Bo phan tuoi nuoc */
    double hmin, hmax, water_vol;
    int water_times[MAX_SLOTS];     // cac thoi diem tuoi co dinh (phut trong ngay)
    int n_water_times;
    double total_water;

    /* Bo phan bon phan NPK */
    double nmin, pmin, kmin;
    double conc;                    // C: gam/lit
    double fert_vol;                // V: lit
    double total_fert;              // tong so gam phan da bon

    /* Den ho tro quang hop */
    double watt;                    // P: cong suat (W)
    int light_on_at[MAX_SLOTS], light_off_at[MAX_SLOTS];
    int n_light_slots;
    long manual_until;              // bat thu cong den phut nay
    int light_state;
    double energy_wh;

    long last_auto;                 // thoi diem tu dong tuoi/bon gan nhat (chong lap)
} Device;

extern Environment g_env;
extern long g_sim_min;              // so phut mo phong da troi qua
extern int g_start_tod;             // gio bat dau mo phong (phut trong ngay)

const char *dev_type_name(DevType t);
int dev_type_parse(const char *s);

void dev_init(Device *d, DevType type, const char *id, const char *password, int port);
int dev_load_state(Device *d);
void dev_save_state(Device *d);
void dev_log(Device *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void env_tick(void);
void dev_tick(Device *d);

/* Xu ly 1 dong lenh. Tra ve 1 neu client yeu cau dong ket noi */
int dev_handle_command(Device *d, int fd, char *line, const char *peer);

#endif
