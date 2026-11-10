/*
 * Luu danh sach vuon va thiet bi ra file text, moi dong 1 ban ghi:
 *   G|<ten vuon>|<vi tri>
 *   D|<ten vuon>|<id>|<loai>|<ip>|<port>|<token hoac ->
 */
#include "garden_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Garden g_gardens[MAX_GARDENS];
int g_ngardens = 0;
CDevice g_devices[MAX_CDEVICES];
int g_ndevices = 0;

static char store_path[256];

int store_load(const char *path){
    snprintf(store_path, sizeof(store_path), "%s", path);
    g_ngardens = g_ndevices = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[8], *p = line;
        int n = 0;
        while (n < 8 && (fields[n] = strsep(&p, "|")) != NULL) n++;

        if (n >= 3 && strcmp(fields[0], "G") == 0) {
            store_add_garden(fields[1], fields[2]);
        } else if (n >= 7 && strcmp(fields[0], "D") == 0) {
            CDevice *d = store_add_device(fields[1], fields[2], fields[3], fields[4], atoi(fields[5]));
            if (d && strcmp(fields[6], "-") != 0) snprintf(d->token, sizeof(d->token), "%s", fields[6]);
        }
    }
    fclose(f);
    return 0;
}

int store_save(void){
    FILE *f = fopen(store_path, "w");
    if (f == NULL) return -1;
    for (int i = 0; i < g_ngardens; i++)
        fprintf(f, "G|%s|%s\n", g_gardens[i].name, g_gardens[i].location);
    for (int i = 0; i < g_ndevices; i++) {
        CDevice *d = &g_devices[i];
        fprintf(f, "D|%s|%s|%s|%s|%d|%s\n", d->garden, d->id, d->type, d->ip, d->port,
                d->token[0] ? d->token : "-");
    }
    fclose(f);
    return 0;
}

Garden *store_find_garden(const char *name){
    for (int i = 0; i < g_ngardens; i++)
        if (strcmp(g_gardens[i].name, name) == 0) return &g_gardens[i];
    return NULL;
}

CDevice *store_find_device(const char *id){
    for (int i = 0; i < g_ndevices; i++)
        if (strcmp(g_devices[i].id, id) == 0) return &g_devices[i];
    return NULL;
}

Garden *store_add_garden(const char *name, const char *location){
    if (store_find_garden(name) || g_ngardens >= MAX_GARDENS) return NULL;
    Garden *g = &g_gardens[g_ngardens++];
    snprintf(g->name, sizeof(g->name), "%s", name);
    snprintf(g->location, sizeof(g->location), "%s", location);
    return g;
}

void store_remove_garden(const char *name){
    for (int i = g_ndevices - 1; i >= 0; i--)
        if (strcmp(g_devices[i].garden, name) == 0) store_remove_device(&g_devices[i]);
    for (int i = 0; i < g_ngardens; i++) {
        if (strcmp(g_gardens[i].name, name) == 0) {
            g_gardens[i] = g_gardens[--g_ngardens];
            return;
        }
    }
}

/* Them moi, hoac cap nhat dia chi neu thiet bi da co trong danh sach */
CDevice *store_add_device(const char *garden, const char *id, const char *type, const char *ip, int port){
    CDevice *d = store_find_device(id);
    if (d == NULL) {
        if (g_ndevices >= MAX_CDEVICES) return NULL;
        d = &g_devices[g_ndevices++];
        memset(d, 0, sizeof(*d));
        snprintf(d->id, sizeof(d->id), "%s", id);
    }
    snprintf(d->garden, sizeof(d->garden), "%s", garden);
    snprintf(d->type, sizeof(d->type), "%s", type);
    snprintf(d->ip, sizeof(d->ip), "%s", ip);
    d->port = port;
    return d;
}

void store_remove_device(CDevice *d){
    int idx = (int)(d - g_devices);
    memmove(&g_devices[idx], &g_devices[idx + 1], (g_ndevices - idx - 1) * sizeof(CDevice));
    g_ndevices--;
}

int store_count_devices(const char *garden){
    int n = 0;
    for (int i = 0; i < g_ndevices; i++)
        if (strcmp(g_devices[i].garden, garden) == 0) n++;
    return n;
}
