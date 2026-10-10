#pragma once
#include <stdint.h>
#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4
struct dirent {
    unsigned char d_type;
    char          d_name[256];
};
typedef struct wfs_dir DIR;
DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
