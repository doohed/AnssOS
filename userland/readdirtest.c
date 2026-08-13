/* Test payload: proves opendir()/readdir()/closedir() reach the real
 * VFS tree -- lists "/" and prints each entry's name and type. A
 * follow-up `ls /` from the shell (see kernel/src/main.c's self-test
 * wiring) confirms the listing matches. */

#include "libc.h"

int main(void) {
    DIR *dirp = opendir("/");
    if (dirp == NULL) {
        printf("opendir(/) failed\n");
        exit(1);
    }

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dirp)) != NULL) {
        printf("%s %s\n", ent->d_type == DT_DIR ? "dir " : "file", ent->d_name);
        count++;
    }
    closedir(dirp);

    printf("readdirtest done: %d entries\n", count);
    exit(0);
}
