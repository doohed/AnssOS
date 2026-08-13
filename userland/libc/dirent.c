/* opendir()/readdir()/closedir()/rewinddir() -- built on top of the
 * simplified one-entry-per-call getdents() syscall (see
 * kernel/src/exec/syscall.c's sys_getdents_impl()). */

#include "../libc.h"

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    DIR *dirp = malloc(sizeof(DIR));
    if (dirp == NULL) {
        close(fd);
        return NULL;
    }
    dirp->fd = fd;
    return dirp;
}

struct dirent *readdir(DIR *dirp) {
    static struct dirent entry; /* Valid until the next call, matching real readdir(). */
    if (getdents(dirp->fd, &entry) <= 0) {
        return NULL;
    }
    return &entry;
}

int closedir(DIR *dirp) {
    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}

void rewinddir(DIR *dirp) {
    lseek(dirp->fd, 0, SEEK_SET);
}
