/* Code to mangle pathnames into those matching a given prefix.
   eg. open("/lib/foo.so") => open("/usr/gnemul/i386-linux/lib/foo.so");

   The assumption is that this area does not change.
*/
#include "qemu/osdep.h"
#include <sys/param.h>
#include <dirent.h>
#include "qemu/cutils.h"
#include "qemu/path.h"
#include "qemu/thread.h"

static const char *base;
static GHashTable *hash;
static QemuMutex lock;

void init_paths(const char *prefix)
{
    if (prefix[0] == '\0' || !strcmp(prefix, "/")) {
        return;
    }

    if (prefix[0] == '/') {
        base = g_strdup(prefix);
    } else {
        char *cwd = g_get_current_dir();
        base = g_build_filename(cwd, prefix, NULL);
        g_free(cwd);
    }

    hash = g_hash_table_new(g_str_hash, g_str_equal);
    qemu_mutex_init(&lock);
}

/* Use the value from include/linux/namei.h if not provided by libc.  */
#ifndef MAXSYMLINKS
#define MAXSYMLINKS 40
#endif

/* Follow symlinks in a loop.  */
static char *follow_symlinks(char *path)
{
    struct stat buf;
    int total_link_count = 0;

    while (lstat(path, &buf) == 0 && S_ISLNK(buf.st_mode)) {
        char *free_me = path;
        char *target;

        /* Detect symlink loops.  */
        total_link_count++;
        if (total_link_count >= MAXSYMLINKS) {
            g_free(free_me);
            return NULL;
        }

        /* Figure out symlink target.  */
        target = g_file_read_link(path, NULL);
        if (target == NULL) {
            g_free(free_me);
            return NULL;
        }

        if (target[0] == '/') {
            /* Absolute target.  */
            path = g_build_filename(base, target, NULL);
        } else {
            /* Relative target.  */
            char *last_slash = g_strrstr(path, "/");

            assert(last_slash != NULL);
            *last_slash = '\0';
            path = g_build_filename(path, target, NULL);
        }

        g_free(target);
        g_free(free_me);
    }
    return path;
}

/* Look for path in emulation dir, otherwise return name. */
const char *path(const char *name)
{
    gpointer key, value;
    const char *ret;

    /* Only do absolute paths: quick and dirty, but should mostly be OK.  */
    if (!base || !name || name[0] != '/') {
        return name;
    }

    qemu_mutex_lock(&lock);

    /* Have we looked up this file before?  */
    if (g_hash_table_lookup_extended(hash, name, &key, &value)) {
        ret = value ? value : name;
    } else {
        char *save = g_strdup(name);
        char *full = g_build_filename(base, name, NULL);

        full = follow_symlinks(full);

        /* Look for the path; record the result, pass or fail.  */
        if (full != NULL && access(full, F_OK) == 0) {
            /* Exists.  */
            g_hash_table_insert(hash, save, full);
            ret = full;
        } else {
            /* Does not exist.  */
            g_free(full);
            g_hash_table_insert(hash, save, NULL);
            ret = name;
        }
    }

    qemu_mutex_unlock(&lock);
    return ret;
}
