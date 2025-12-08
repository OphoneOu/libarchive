/*
 * Pack/unpack example that gathers file attributes using POSIX APIs
 * (lstat/opendir/readlink) instead of archive_read_disk. Compression
 * filters remain disabled for a plain ustar stream.
 */

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char buffer[16384];

static int
copy_data(struct archive *ar, struct archive *aw)
{
    int r;
    const void *buff;
    size_t size;
    la_int64_t offset;

    for (;;) {
        r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF)
            return ARCHIVE_OK;
        if (r != ARCHIVE_OK)
            return r;
        r = archive_write_data_block(aw, buff, size, offset);
        if (r != ARCHIVE_OK)
            return r;
    }
}

static int
write_file_data(struct archive *a, const char *src)
{
    int fd = open(src, O_RDONLY);
    ssize_t len;
    if (fd < 0)
        return ARCHIVE_FAILED;

    while ((len = read(fd, buffer, sizeof(buffer))) > 0) {
        if (archive_write_data(a, buffer, (size_t)len) < 0) {
            close(fd);
            return ARCHIVE_FAILED;
        }
    }
    close(fd);
    return ARCHIVE_OK;
}

static int
append_path(struct archive *aw, const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        perror(path);
        return 1;
    }

    struct archive_entry *entry = archive_entry_new();
    archive_entry_copy_pathname(entry, path);
    archive_entry_set_perm(entry, st.st_mode & 07777);
    archive_entry_set_uid(entry, st.st_uid);
    archive_entry_set_gid(entry, st.st_gid);
    archive_entry_set_mtime(entry, st.st_mtime, 0);

    if (S_ISREG(st.st_mode)) {
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_size(entry, st.st_size);
    } else if (S_ISDIR(st.st_mode)) {
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_size(entry, 0);
    } else if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len < 0) {
            perror("readlink");
            archive_entry_free(entry);
            return 1;
        }
        target[len] = '\0';
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_size(entry, 0);
        archive_entry_copy_symlink(entry, target);
    } else {
        /* Skip unsupported types to keep the example short. */
        fprintf(stderr, "skip unsupported: %s\n", path);
        archive_entry_free(entry);
        return 0;
    }

    int rc = archive_write_header(aw, entry);
    if (rc > ARCHIVE_FAILED && S_ISREG(st.st_mode) && st.st_size > 0)
        rc = write_file_data(aw, path);

    archive_entry_free(entry);
    if (rc != ARCHIVE_OK) {
        fprintf(stderr, "write header/data failed for %s: %s\n", path,
                archive_error_string(aw));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            perror(path);
            return 1;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (append_path(aw, child) != 0) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }

    return 0;
}

static int
pack_paths_posix(const char *output, const char **paths)
{
    struct archive *aw = archive_write_new();
    int rc = 0;

    archive_write_add_filter_none(aw);
    archive_write_set_format_ustar(aw);

    if (archive_write_open_filename(aw, output) != ARCHIVE_OK) {
        fprintf(stderr, "open %s failed: %s\n", output,
                archive_error_string(aw));
        archive_write_free(aw);
        return 1;
    }

    for (; *paths != NULL; ++paths) {
        if (append_path(aw, *paths) != 0) {
            rc = 1;
            break;
        }
    }

    archive_write_close(aw);
    archive_write_free(aw);
    return rc;
}

static int
extract_to(const char *archive_path, const char *dest)
{
    struct archive *ar = archive_read_new();
    struct archive *aw = archive_write_disk_new();
    struct archive_entry *entry;
    int rc;

    archive_read_support_filter_none(ar);
    archive_read_support_format_tar(ar);

    archive_write_disk_set_standard_lookup(aw);
    archive_write_disk_set_options(aw, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                         ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);

    if ((rc = archive_read_open_filename(ar, archive_path, 10240))) {
        fprintf(stderr, "open archive failed: %s\n", archive_error_string(ar));
        archive_read_free(ar);
        archive_write_free(aw);
        return 1;
    }

    while ((rc = archive_read_next_header(ar, &entry)) != ARCHIVE_EOF) {
        if (rc != ARCHIVE_OK) {
            fprintf(stderr, "header error: %s\n", archive_error_string(ar));
            break;
        }

        const char *current = archive_entry_pathname(entry);
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dest, current);
        archive_entry_set_pathname(entry, fullpath);

        rc = archive_write_header(aw, entry);
        if (rc == ARCHIVE_OK && archive_entry_size(entry) > 0)
            rc = copy_data(ar, aw);

        if (rc != ARCHIVE_OK) {
            fprintf(stderr, "extract error for %s: %s\n", fullpath,
                    archive_error_string(aw));
            break;
        }
        archive_write_finish_entry(aw);
    }

    archive_read_close(ar);
    archive_read_free(ar);
    archive_write_close(aw);
    archive_write_free(aw);
    return rc == ARCHIVE_EOF ? 0 : 1;
}

int
main(int argc, const char **argv)
{
    /*
     * Usage:
     *   ./posix_attr_examples c output.tar path1 [path2 ...]
     *   ./posix_attr_examples x archive.tar destination_dir
     * Build (no gzip/bzip2 dependencies):
     *   cc -D_FILE_OFFSET_BITS=64 -o posix_attr_examples posix_attr_examples.c \
     *      -larchive
     */
    if (argc < 3) {
        fprintf(stderr,
                "Usage:\n  %s c <output.tar> <path...>\n  %s x <archive.tar> <dest_dir>\n",
                argv[0], argv[0]);
        return 1;
    }

    if (argv[1][0] == 'c')
        return pack_paths_posix(argv[2], argv + 3);
    if (argv[1][0] == 'x' && argc == 4)
        return extract_to(argv[2], argv[3]);

    fprintf(stderr, "invalid arguments\n");
    return 1;
}
