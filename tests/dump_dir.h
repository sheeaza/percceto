#ifndef DUMP_DIR_H
#define DUMP_DIR_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Creates @dir if needed and empties it, so a test that writes dumps into it
 * never sees files left by an earlier run. */
static inline void dir_reset(const char *dir)
{
    mkdir(dir, 0755);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

#endif /* DUMP_DIR_H */
