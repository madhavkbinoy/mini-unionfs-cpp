/*
 * main.cpp — Entry point for Mini-UnionFS
 *
 * Validates command-line arguments, constructs the UnionFs, and hands off
 * to fuse_main().
 */

#include "unionfs.h"

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s <lower_dir> <upper_dir> <mount_dir>\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "  lower_dir   Read-only base image layer\n");
    fprintf(stderr, "  upper_dir   Read-write container layer\n");
    fprintf(stderr, "  mount_dir   FUSE mount point (unified view)\n");
}

static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    /* --- Resolve and validate both layer roots --- */
    char *lower_real = realpath(argv[1], NULL);
    if (!lower_real) {
        fprintf(stderr, "Error: lower_dir '%s' does not exist\n", argv[1]);
        return 1;
    }
    char *upper_real = realpath(argv[2], NULL);
    if (!upper_real) {
        fprintf(stderr, "Error: upper_dir '%s' does not exist\n", argv[2]);
        free(lower_real);
        return 1;
    }
    if (!is_directory(lower_real)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", lower_real);
        free(lower_real);
        free(upper_real);
        return 1;
    }
    if (!is_directory(upper_real)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", upper_real);
        free(lower_real);
        free(upper_real);
        return 1;
    }

    fprintf(stderr, "Mini-UnionFS mounting...\n");
    fprintf(stderr, "  lower: %s\n", lower_real);
    fprintf(stderr, "  upper: %s\n", upper_real);
    fprintf(stderr, "  mount: %s\n", argv[3]);

    /* --- Construct the UnionFs (copies the paths; realpath()'s buffers
     * are no longer needed once the std::string members are built) --- */
    UnionFs *fs = new UnionFs(lower_real, upper_real);
    free(lower_real);
    free(upper_real);

    /*
     * Build FUSE argument vector.
     * -f = foreground mode (keeps stderr visible for debugging).
     * Remove -f for production / daemonized use.
     */
    char *fuse_argv[] = { argv[0], argv[3], NULL };
    int fuse_argc = 2;

    int ret = fuse_main(fuse_argc, fuse_argv, &unionfs_ops(), fs);

    /* --- Cleanup --- */
    delete fs;
    return ret;
}
