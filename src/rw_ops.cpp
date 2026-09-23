/*
 * rw_ops.cpp — Write-side FUSE callbacks & Copy-on-Write engine
 */

#include "unionfs.h"

/* 64 KB copy buffer as specified */
#define COW_BUF_SIZE (64 * 1024)

/* mkdir -p helper. Free function, not a UnionFs method: it has no notion
 * of upper/lower, it just mutates an already-resolved host path. */
int make_parent_dirs(const char *full_path) {
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, full_path, MAX_PATH_LEN);
    tmp[MAX_PATH_LEN - 1] = '\0';

    char *last_slash = strrchr(tmp, '/');
    if (!last_slash || last_slash == tmp) return 0;

    for (char *p = tmp + 1; p < last_slash; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -errno;
            *p = '/';
        }
    }

    *last_slash = '\0';
    if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -errno;
    return 0;
}

/* Copy-on-Write engine */
int UnionFs::cow_copy(const char *path) {
    std::string src = lower_path(path);
    std::string dst = upper_path(path);

    Fd src_fd(::open(src.c_str(), O_RDONLY));
    if (!src_fd.valid()) return sys_error();

    struct stat st;
    if (::fstat(src_fd.get(), &st) == -1) return sys_error();

    if (make_parent_dirs(dst.c_str()) < 0) return sys_error();

    Fd dst_fd(::open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, st.st_mode));
    if (!dst_fd.valid()) return sys_error();

    char buf[COW_BUF_SIZE];
    ssize_t bytes;
    while ((bytes = ::read(src_fd.get(), buf, COW_BUF_SIZE)) > 0) {
        ssize_t written = 0;
        while (written < bytes) {
            ssize_t w = ::write(dst_fd.get(), buf + written, bytes - written);
            if (w == -1) return sys_error();
            written += w;
        }
    }

    ::fchmod(dst_fd.get(), st.st_mode);
    return 0;   // src_fd, dst_fd close automatically here
}

int UnionFs::open(const char *path, struct fuse_file_info *fi) {
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (::access(upper.c_str(), F_OK) != 0) {
            if (::access(lower.c_str(), F_OK) == 0) {
                int ret = cow_copy(path);
                if (ret < 0) return ret;
            }
        }
    }
    return resolve_path(path) ? 0 : -ENOENT;
}

int UnionFs::write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    std::string upper = upper_path(path);

    Fd fd(::open(upper.c_str(), O_WRONLY));
    if (!fd.valid()) return sys_error();

    ssize_t res = ::pwrite(fd.get(), buf, size, offset);
    if (res == -1) res = sys_error();

    return static_cast<int>(res);   // fd closes automatically
}

int UnionFs::create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    std::string upper = upper_path(path);
    if (make_parent_dirs(upper.c_str()) < 0) return sys_error();

    /* Clear stale whiteout marker so the re-created file becomes visible */
    std::string wh_path = whiteout_path(path);
    if (::access(wh_path.c_str(), F_OK) == 0)
        ::unlink(wh_path.c_str());

    Fd fd(::open(upper.c_str(), O_CREAT | O_WRONLY | O_TRUNC, mode));
    if (!fd.valid()) return sys_error();

    fi->fh = static_cast<uint64_t>(fd.release());   // ownership transfers to fi->fh
    return 0;
}

int UnionFs::truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void) fi;
    std::string upper = upper_path(path);
    std::string lower = lower_path(path);

    if (::access(upper.c_str(), F_OK) != 0) {
        if (::access(lower.c_str(), F_OK) == 0) {
            int ret = cow_copy(path);
            if (ret < 0) return ret;
        } else {
            return -ENOENT;
        }
    }

    if (::truncate(upper.c_str(), size) == -1) return sys_error();
    return 0;
}

int UnionFs::utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void) fi;
    auto resolved = resolve_path(path);
    if (!resolved) return -ENOENT;

    if (::utimensat(AT_FDCWD, resolved->c_str(), tv, AT_SYMLINK_NOFOLLOW) == -1)
        return sys_error();
    return 0;
}

int UnionFs::release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    if (fi->fh) {
        Fd(static_cast<int>(fi->fh));   // closes immediately: temporary destroyed here
        fi->fh = 0;
    }
    return 0;
}
