#ifndef GARDEN_STORE_H
#define GARDEN_STORE_H

#define MAX_GARDENS 32
#define MAX_CDEVICES 128

typedef struct {
    char name[32];
    char location[64];
} Garden;

/* Thiet bi da duoc them vao 1 vuon */
typedef struct {
    char garden[32];
    char id[32];
    char type[16];
    char ip[64];
    int port;
    char token[40];       // rong neu chua ket noi
} CDevice;

extern Garden g_gardens[MAX_GARDENS];
extern int g_ngardens;
extern CDevice g_devices[MAX_CDEVICES];
extern int g_ndevices;

int store_load(const char *path);
int store_save(void);

Garden *store_find_garden(const char *name);
CDevice *store_find_device(const char *id);
Garden *store_add_garden(const char *name, const char *location);
void store_remove_garden(const char *name);
CDevice *store_add_device(const char *garden, const char *id, const char *type, const char *ip, int port);
void store_remove_device(CDevice *d);
int store_count_devices(const char *garden);

#endif
