/* stdio / POSIX file calls (see watch_fs.h) on Zephyr fs_*. */
#define WFS_IMPL
#include "watch_fs.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#define FD_BASE 1000

struct wfs_file {
    struct fs_file_t f;
    int  idx;
    bool used;
};

static struct wfs_file s_files[8];
static K_MUTEX_DEFINE(s_lock);

FILE *wfs_fopen(const char *path, const char *mode)
{
    fs_mode_t m = 0;
    bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': m = plus ? FS_O_RDWR : FS_O_READ; break;
    case 'w': m = (plus ? FS_O_RDWR : FS_O_WRITE) | FS_O_CREATE; break;
    case 'a': m = (plus ? FS_O_RDWR : FS_O_WRITE) | FS_O_CREATE | FS_O_APPEND; break;
    default:  errno = EINVAL; return NULL;
    }
    k_mutex_lock(&s_lock, K_FOREVER);
    struct wfs_file *wf = NULL;
    for (int i = 0; i < ARRAY_SIZE(s_files); i++) {
        if (!s_files[i].used) { wf = &s_files[i]; wf->idx = i; break; }
    }
    if (!wf) { k_mutex_unlock(&s_lock); errno = EMFILE; return NULL; }
    fs_file_t_init(&wf->f);
    if (mode[0] == 'w') fs_unlink(path);     /* truncate: LittleFS here has no O_TRUNC */
    int r = fs_open(&wf->f, path, m);
    if (r < 0) { k_mutex_unlock(&s_lock); errno = -r; return NULL; }
    wf->used = true;
    k_mutex_unlock(&s_lock);
    return (FILE *)wf;
}

int wfs_fclose(FILE *f)
{
    struct wfs_file *wf = (struct wfs_file *)f;
    int r = fs_close(&wf->f);
    k_mutex_lock(&s_lock, K_FOREVER);
    wf->used = false;
    k_mutex_unlock(&s_lock);
    return r < 0 ? EOF : 0;
}

size_t wfs_fread(void *buf, size_t size, size_t n, FILE *f)
{
    if (!size || !n) return 0;
    ssize_t r = fs_read(&((struct wfs_file *)f)->f, buf, size * n);
    return r <= 0 ? 0 : (size_t)r / size;
}

size_t wfs_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (!size || !n) return 0;
    ssize_t r = fs_write(&((struct wfs_file *)f)->f, buf, size * n);
    return r <= 0 ? 0 : (size_t)r / size;
}

int wfs_fseek(FILE *f, long off, int whence)
{
    int w = whence == SEEK_SET ? FS_SEEK_SET : whence == SEEK_CUR ? FS_SEEK_CUR : FS_SEEK_END;
    return fs_seek(&((struct wfs_file *)f)->f, off, w) < 0 ? -1 : 0;
}

long wfs_ftell(FILE *f)
{
    return (long)fs_tell(&((struct wfs_file *)f)->f);
}

int wfs_fflush(FILE *f)
{
    if (!f) return 0;
    return fs_sync(&((struct wfs_file *)f)->f) < 0 ? EOF : 0;
}

int wfs_fileno(FILE *f)
{
    return FD_BASE + ((struct wfs_file *)f)->idx;
}

int wfs_fstat(int fd, struct stat *st)
{
    if (fd < FD_BASE || fd >= FD_BASE + (int)ARRAY_SIZE(s_files) || !s_files[fd - FD_BASE].used) {
        errno = EBADF;
        return -1;
    }
    struct fs_file_t *f = &s_files[fd - FD_BASE].f;
    off_t pos = fs_tell(f);
    fs_seek(f, 0, FS_SEEK_END);
    off_t size = fs_tell(f);
    fs_seek(f, pos, FS_SEEK_SET);
    memset(st, 0, sizeof(*st));
    st->st_size = size;
    st->st_mode = S_IFREG | 0644;
    return 0;
}

int wfs_stat(const char *path, struct stat *st)
{
    struct fs_dirent e;
    int r = fs_stat(path, &e);
    if (r < 0) { errno = -r; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = e.size;
    st->st_mode = e.type == FS_DIR_ENTRY_DIR ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    return 0;
}

int wfs_mkdir(const char *path, mode_t mode)
{
    int r = fs_mkdir(path);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int wfs_unlink(const char *path)
{
    int r = fs_unlink(path);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int wfs_rename(const char *from, const char *to)
{
    int r = fs_rename(from, to);
    if (r == -EEXIST) {               /* POSIX rename replaces the target */
        fs_unlink(to);
        r = fs_rename(from, to);
    }
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

/* ---- directories ---- */

struct wfs_dir {
    struct fs_dir_t d;
    struct dirent   ent;
};

DIR *opendir(const char *path)
{
    struct wfs_dir *d = malloc(sizeof(*d));
    if (!d) { errno = ENOMEM; return NULL; }
    fs_dir_t_init(&d->d);
    int r = fs_opendir(&d->d, path);
    if (r < 0) { free(d); errno = -r; return NULL; }
    return d;
}

struct dirent *readdir(DIR *d)
{
    struct fs_dirent e;
    if (fs_readdir(&d->d, &e) < 0 || !e.name[0]) return NULL;
    strncpy(d->ent.d_name, e.name, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
    d->ent.d_type = e.type == FS_DIR_ENTRY_DIR ? DT_DIR : DT_REG;
    return &d->ent;
}

int closedir(DIR *d)
{
    fs_closedir(&d->d);
    free(d);
    return 0;
}
