/* stdio / POSIX file calls on Zephyr's fs_* API (LittleFS at /data).
 * Force-included into the app sources, after the libc headers it overrides. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "dirent.h"

FILE  *wfs_fopen(const char *path, const char *mode);
int    wfs_fclose(FILE *f);
size_t wfs_fread(void *buf, size_t size, size_t n, FILE *f);
size_t wfs_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    wfs_fseek(FILE *f, long off, int whence);
long   wfs_ftell(FILE *f);
int    wfs_fflush(FILE *f);
int    wfs_fileno(FILE *f);
int    wfs_fstat(int fd, struct stat *st);
int    wfs_stat(const char *path, struct stat *st);
int    wfs_mkdir(const char *path, mode_t mode);
int    wfs_unlink(const char *path);
int    wfs_rename(const char *from, const char *to);

#ifndef WFS_IMPL
#define fopen(p, m)          wfs_fopen(p, m)
#define fclose(f)            wfs_fclose(f)
#define fread(b, s, n, f)    wfs_fread(b, s, n, f)
#define fwrite(b, s, n, f)   wfs_fwrite(b, s, n, f)
#define fseek(f, o, w)       wfs_fseek(f, o, w)
#define ftell(f)             wfs_ftell(f)
#define fflush(f)            wfs_fflush(f)
#define fileno(f)            wfs_fileno(f)
#define fstat(fd, st)        wfs_fstat(fd, st)
#define stat(p, st)          wfs_stat(p, st)
#define mkdir(p, m)          wfs_mkdir(p, m)
#define unlink(p)            wfs_unlink(p)
#define remove(p)            wfs_unlink(p)
#define rmdir(p)             wfs_unlink(p)
#define rename(a, b)         wfs_rename(a, b)
#endif
