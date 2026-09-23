/*
 * del_ops.cpp — Deletion, directory, metadata, and link FUSE callbacks
 *
 * Contains UnionFs::{unlink, mkdir, rmdir, chmod, chown, statfs, symlink,
 * readlink, rename, link}, the extern "C" shims that bridge every FUSE
 * operation into fuse_guard(), and make_ops()/unionfs_ops() (the dispatch
 * table).
 */

#include "unionfs.h"

/* 3.1 — UnionFs::unlink() — The Whiteout Engine */
int UnionFs::unlink(const char *path) {
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    // Edge Case 4.1: Check if visible
    if (!resolve_path(path)) {
        return -ENOENT;
    }

    int in_upper = (::access(upper.c_str(), F_OK) == 0);
    int in_lower = (::access(lower.c_str(), F_OK) == 0);

    // PATH A: Exists ONLY in upper
    if (in_upper && !in_lower) {
        if (::unlink(upper.c_str()) == -1) return sys_error();
        return 0;
    }

    // PATH B: Originates from lower_dir
    if (in_lower) {
        std::string wh_path = whiteout_path(path);

        if (make_parent_dirs(wh_path.c_str()) < 0) return sys_error();

        // Create marker with 0000 permissions as system marker
        Fd fd(::open(wh_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0000));
        if (!fd.valid()) return sys_error();
        fd.reset();   // close it now, matching the original's immediate close()

        // Edge Case 4.2: If CoW copy exists, remove it
        if (in_upper) ::unlink(upper.c_str());

        return 0;
    }

    return -ENOENT;
}

/* 3.2 — UnionFs::mkdir() */
int UnionFs::mkdir(const char *path, mode_t mode) {
    // Edge Case 4.5: Remove stale whiteouts
    std::string wh_check = whiteout_path(path);
    if (::access(wh_check.c_str(), F_OK) == 0) ::unlink(wh_check.c_str());

    std::string upper = upper_path(path);
    if (make_parent_dirs(upper.c_str()) < 0) return sys_error();
    if (::mkdir(upper.c_str(), mode) == -1) return sys_error();
    return 0;
}

/* 3.3 — UnionFs::rmdir() */
int UnionFs::rmdir(const char *path) {
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    if (::access(upper.c_str(), F_OK) == 0) {
        if (::rmdir(upper.c_str()) == -1) return sys_error();
        // If it also exists in lower, we need a whiteout to hide it
        if (::access(lower.c_str(), F_OK) == 0) {
            std::string wh_path = whiteout_path(path);
            Fd fd(::open(wh_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0000));
            // fd closes automatically; open() failure is silently ignored,
            // matching the original (no error path here)
        }
        return 0;
    }
    // Edge Case 4.4: Lower-only directory limitation
    if (::access(lower.c_str(), F_OK) == 0) return -EPERM;

    return -ENOENT;
}

/* 3.4 & 3.5 — Metadata ops with CoW */
int UnionFs::chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) fi;
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    if (::access(upper.c_str(), F_OK) != 0) {
        if (::access(lower.c_str(), F_OK) == 0) {
            int ret = cow_copy(path);
            if (ret < 0) return ret;
        } else return -ENOENT;
    }
    if (::chmod(upper.c_str(), mode) == -1) return sys_error();
    return 0;
}

int UnionFs::chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void) fi;
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    if (::access(upper.c_str(), F_OK) != 0) {
        if (::access(lower.c_str(), F_OK) == 0) {
            int ret = cow_copy(path);
            if (ret < 0) return ret;
        } else return -ENOENT;
    }
    if (::lchown(upper.c_str(), uid, gid) == -1) return sys_error();
    return 0;
}

int UnionFs::statfs(const char *path, struct statvfs *stbuf) {
    (void) path;
    if (::statvfs(upper_dir().c_str(), stbuf) == -1)
        return sys_error();
    return 0;
}

int UnionFs::symlink(const char *target, const char *linkpath) {
    std::string upper_link = upper_path(linkpath);

    if (make_parent_dirs(upper_link.c_str()) < 0) return sys_error();
    if (::symlink(target, upper_link.c_str()) == -1) return sys_error();
    return 0;
}

int UnionFs::readlink(const char *path, char *buf, size_t size) {
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);
    char linkbuf[MAX_PATH_LEN];
    ssize_t len;

    if (::access(upper.c_str(), F_OK) == 0) {
        len = ::readlink(upper.c_str(), linkbuf, size - 1);
    } else if (::access(lower.c_str(), F_OK) == 0) {
        len = ::readlink(lower.c_str(), linkbuf, size - 1);
    } else {
        return -ENOENT;
    }

    if (len == -1) return sys_error();
    linkbuf[len] = '\0';
    strncpy(buf, linkbuf, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

int UnionFs::rename(const char *from, const char *to, unsigned int flags) {
    (void) flags;
    std::string upper_from = upper_path(from);
    std::string upper_to = upper_path(to);
    std::string lower_from = lower_path(from);
    int ret;

    if (::access(upper_from.c_str(), F_OK) == 0) {
        if (make_parent_dirs(upper_to.c_str()) < 0) return sys_error();
        if (::rename(upper_from.c_str(), upper_to.c_str()) == -1) return sys_error();
        return 0;
    }

    if (::access(lower_from.c_str(), F_OK) == 0) {
        ret = cow_copy(from);
        if (ret < 0) return ret;
        if (make_parent_dirs(upper_to.c_str()) < 0) return sys_error();
        if (::rename(upper_from.c_str(), upper_to.c_str()) == -1) return sys_error();
        return 0;
    }

    return -ENOENT;
}

int UnionFs::link(const char *from, const char *to) {
    std::string upper_from = upper_path(from);
    std::string upper_to = upper_path(to);
    std::string lower_from = lower_path(from);
    int ret;

    if (::access(upper_from.c_str(), F_OK) != 0) {
        if (::access(lower_from.c_str(), F_OK) == 0) {
            ret = cow_copy(from);
            if (ret < 0) return ret;
        } else {
            return -ENOENT;
        }
    }

    if (make_parent_dirs(upper_to.c_str()) < 0) return sys_error();
    if (::link(upper_from.c_str(), upper_to.c_str()) == -1) return sys_error();
    return 0;
}

/* ================================================================
 * extern "C" shims — every FUSE callback body is exactly one
 * fuse_guard(...) call wrapping the matching UnionFs method.
 *
 * libfuse3 is a C library: letting a C++ exception unwind into it is
 * undefined behaviour. This is the one and only place a thrown exception
 * is allowed to be caught in this codebase — see fuse_guard() in
 * unionfs.h. fs_ctx() is fetched fresh on every call; it is never cached.
 * ================================================================ */

extern "C" {

static int c_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().getattr(path, stbuf, fi); });
}
static int c_readlink(const char *path, char *buf, size_t size) {
    return fuse_guard([&] { return fs_ctx().readlink(path, buf, size); });
}
static int c_mkdir(const char *path, mode_t mode) {
    return fuse_guard([&] { return fs_ctx().mkdir(path, mode); });
}
static int c_unlink(const char *path) {
    return fuse_guard([&] { return fs_ctx().unlink(path); });
}
static int c_rmdir(const char *path) {
    return fuse_guard([&] { return fs_ctx().rmdir(path); });
}
static int c_symlink(const char *target, const char *linkpath) {
    return fuse_guard([&] { return fs_ctx().symlink(target, linkpath); });
}
static int c_rename(const char *from, const char *to, unsigned int flags) {
    return fuse_guard([&] { return fs_ctx().rename(from, to, flags); });
}
static int c_link(const char *from, const char *to) {
    return fuse_guard([&] { return fs_ctx().link(from, to); });
}
static int c_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().chmod(path, mode, fi); });
}
static int c_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().chown(path, uid, gid, fi); });
}
static int c_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().truncate(path, size, fi); });
}
static int c_open(const char *path, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().open(path, fi); });
}
static int c_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().read(path, buf, size, offset, fi); });
}
static int c_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().write(path, buf, size, offset, fi); });
}
static int c_statfs(const char *path, struct statvfs *stbuf) {
    return fuse_guard([&] { return fs_ctx().statfs(path, stbuf); });
}
static int c_release(const char *path, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().release(path, fi); });
}
static int c_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                     struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    return fuse_guard([&] { return fs_ctx().readdir(path, buf, filler, offset, fi, flags); });
}
static int c_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().create(path, mode, fi); });
}
static int c_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().utimens(path, tv, fi); });
}

} // extern "C"

/* 3.6 — FUSE Dispatch Table
 *
 * Built via value-init + assignment rather than a designated initializer:
 * order-independent (fuse_operations declares its members in a different
 * order than this table's logical grouping, which C++20 designated
 * initializers would require matching), and every unlisted member is
 * value-initialized to nullptr with no -Wmissing-field-initializers noise.
 */
static struct fuse_operations make_ops() noexcept {
    struct fuse_operations ops{};
    ops.getattr  = c_getattr;
    ops.readlink = c_readlink;
    ops.mkdir    = c_mkdir;
    ops.unlink   = c_unlink;
    ops.rmdir    = c_rmdir;
    ops.symlink  = c_symlink;
    ops.rename   = c_rename;
    ops.link     = c_link;
    ops.chmod    = c_chmod;
    ops.chown    = c_chown;
    ops.truncate = c_truncate;
    ops.open     = c_open;
    ops.read     = c_read;
    ops.write    = c_write;
    ops.statfs   = c_statfs;
    ops.release  = c_release;
    ops.readdir  = c_readdir;
    ops.create   = c_create;
    ops.utimens  = c_utimens;
    return ops;
}

const struct fuse_operations &unionfs_ops() {
    static const struct fuse_operations ops = make_ops();
    return ops;
}
