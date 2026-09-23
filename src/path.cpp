/*
 * path.cpp — Path resolution and read-side FUSE callbacks
 *
 * Contains UnionFs::whiteout_path (the whiteout-path builder),
 * UnionFs::resolve_path (the central lookup function), plus
 * getattr, readdir, and read.
 */

#include "unionfs.h"

/* ================================================================
 * UnionFs::whiteout_path — the single whiteout-path builder
 * ================================================================ */

std::string UnionFs::whiteout_path(std::string_view v) const {
    auto slash = v.rfind('/');
    std::string_view dir, base;
    if (slash != std::string_view::npos && slash != 0) {
        /* path has a directory component, e.g. "/subdir/file.txt" */
        dir = v.substr(0, slash);
        base = v.substr(slash + 1);
    } else if (slash == 0) {
        /* path is "/file.txt" (directly in root) */
        base = v.substr(1);
    } else {
        /* bare filename (shouldn't happen in FUSE, but handle it) */
        base = v;
    }
    std::string result(upper_dir());
    result += dir;
    result += '/';
    result += WH_PREFIX;
    result += base;
    return result;
}

/* ================================================================
 * UnionFs::resolve_path — the single most important function in the project
 * ================================================================ */

std::optional<std::string> UnionFs::resolve_path(const char *path) const {
    /* Root directory always exists — serve from upper */
    if (strcmp(path, "/") == 0)
        return upper_dir();

    /* Step 1: Check for whiteout marker */
    std::string whiteout = whiteout_path(path);
    if (::access(whiteout.c_str(), F_OK) == 0)
        return std::nullopt;

    /* Step 2: Check upper layer */
    std::string upper = upper_path(path);
    if (::access(upper.c_str(), F_OK) == 0)
        return upper;

    /* Step 3: Check lower layer */
    std::string lower = lower_path(path);
    if (::access(lower.c_str(), F_OK) == 0)
        return lower;

    /* Step 4: Not found */
    return std::nullopt;
}

/* ================================================================
 * UnionFs::getattr
 * ================================================================ */

int UnionFs::getattr(const char *path, struct stat *stbuf,
                     struct fuse_file_info *fi) {
    (void) fi;
    memset(stbuf, 0, sizeof(struct stat));

    auto resolved = resolve_path(path);
    if (!resolved)
        return -ENOENT;

    if (::lstat(resolved->c_str(), stbuf) == -1)
        return sys_error();

    return 0;
}

/* ================================================================
 * UnionFs::readdir — merges both layers, deduplicates, hides whiteouts
 * ================================================================ */

/* Check if a whiteout exists for 'name' inside 'dir_path' (virtual). */
static bool has_whiteout(const UnionFs &fs, const char *dir_path, const char *name) {
    std::string child = (strcmp(dir_path, "/") == 0)
        ? std::string("/") + name
        : std::string(dir_path) + "/" + name;
    return ::access(fs.whiteout_path(child).c_str(), F_OK) == 0;
}

int UnionFs::readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi,
                     enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;

    struct dirent *de;

    /* seen is LOCAL — a fresh set for every readdir call.
     * Never make this a global: concurrent calls would corrupt it. */
    std::unordered_set<std::string> seen;

    filler(buf, ".", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
    seen.insert(".");
    seen.insert("..");

    /* --- Pass 1: Upper layer (takes precedence) --- */
    std::string dir_path = upper_path(path);
    if (Dir dp(::opendir(dir_path.c_str())); dp.valid()) {
        while ((de = ::readdir(dp.get())) != NULL) {
            const char *name = de->d_name;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            /* Hide whiteout markers from directory listings */
            if (strncmp(name, WH_PREFIX, WH_PREFIX_LEN) == 0)
                continue;
            if (seen.insert(name).second) {
                filler(buf, name, NULL, 0, static_cast<fuse_fill_dir_flags>(0));
            }
        }
    }

    /* --- Pass 2: Lower layer (fill gaps, skip whited-out entries) --- */
    dir_path = lower_path(path);
    if (Dir dp(::opendir(dir_path.c_str())); dp.valid()) {
        while ((de = ::readdir(dp.get())) != NULL) {
            const char *name = de->d_name;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (seen.count(name))
                continue;
            if (has_whiteout(*this, path, name))
                continue;

            filler(buf, name, NULL, 0, static_cast<fuse_fill_dir_flags>(0));
            seen.insert(name);
        }
    }

    return 0;
}

/* ================================================================
 * UnionFs::read
 * ================================================================ */

int UnionFs::read(const char *path, char *buf, size_t size,
                  off_t offset, struct fuse_file_info *fi) {
    (void) fi;

    auto resolved = resolve_path(path);
    if (!resolved)
        return -ENOENT;

    Fd fd(::open(resolved->c_str(), O_RDONLY));
    if (!fd.valid())
        return sys_error();

    int bytes_read = ::pread(fd.get(), buf, size, offset);
    if (bytes_read == -1)
        bytes_read = sys_error();

    return bytes_read;   // fd closes automatically
}
