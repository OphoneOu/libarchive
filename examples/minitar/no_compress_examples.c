/*
 * Minimal pack/unpack examples that only rely on libarchive core and
 * disable gzip/bzip2/other compression filters.  Both paths demonstrate
 * using the ustar format and walking regular files or directories.
 */

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
pack_paths(const char *output, const char **paths)
{
    struct archive *aw = archive_write_new();
    struct archive *disk = NULL;
    struct archive_entry *entry;
    int rc = ARCHIVE_OK;

    /* No compression filters: we only emit a plain ustar stream. */
    archive_write_add_filter_none(aw);
    archive_write_set_format_ustar(aw);

    if (archive_write_open_filename(aw, output) != ARCHIVE_OK) {
        fprintf(stderr, "open %s failed: %s\n", output,
                archive_error_string(aw));
        archive_write_free(aw);
        return 1;
    }

    for (; *paths != NULL; ++paths) {
        disk = archive_read_disk_new();
        archive_read_disk_set_standard_lookup(disk);

        if (archive_read_disk_open(disk, *paths) != ARCHIVE_OK) {
            fprintf(stderr, "disk open %s failed: %s\n", *paths,
                    archive_error_string(disk));
            rc = 1;
            break;
        }

        for (;;) {
            entry = archive_entry_new();
            rc = archive_read_next_header2(disk, entry);
            if (rc == ARCHIVE_EOF) {
                archive_entry_free(entry);
                rc = ARCHIVE_OK;
                break;
            }
            if (rc != ARCHIVE_OK) {
                fprintf(stderr, "read header failed: %s\n",
                        archive_error_string(disk));
                archive_entry_free(entry);
                rc = 1;
                break;
            }

            archive_read_disk_descend(disk);
            rc = archive_write_header(aw, entry);
            if (rc > ARCHIVE_FAILED && archive_entry_size(entry) > 0) {
                rc = write_file_data(aw, archive_entry_sourcepath(entry));
            }
            archive_entry_free(entry);
            if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
                fprintf(stderr, "write header/data failed: %s\n",
                        archive_error_string(aw));
                rc = 1;
                break;
            }
        }
        archive_read_close(disk);
        archive_read_free(disk);
        if (rc != ARCHIVE_OK)
            break;
    }

    archive_write_close(aw);
    archive_write_free(aw);
    return rc == ARCHIVE_OK ? 0 : 1;
}

static int
extract_to(const char *archive_path, const char *dest)
{
    struct archive *ar = archive_read_new();
    struct archive *aw = archive_write_disk_new();
    struct archive_entry *entry;
    int rc;

    /* Only tar reader and no compression filters. */
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
        if (rc == ARCHIVE_OK && archive_entry_size(entry) > 0) {
            rc = copy_data(ar, aw);
        }
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
     *   ./no_compress_examples c output.tar path1 [path2 ...]
     *   ./no_compress_examples x archive.tar destination_dir
     * Build (no gzip/bzip2 dependencies):
     *   cc -D_FILE_OFFSET_BITS=64 -o no_compress_examples no_compress_examples.c \
     *      -larchive
     */
    if (argc < 3) {
        fprintf(stderr,
                "Usage:\n  %s c <output.tar> <path...>\n  %s x <archive.tar> <dest_dir>\n",
                argv[0], argv[0]);
        return 1;
    }

    if (argv[1][0] == 'c') {
        return pack_paths(argv[2], argv + 3);
    } else if (argv[1][0] == 'x' && argc == 4) {
        return extract_to(argv[2], argv[3]);
    }

    fprintf(stderr, "invalid arguments\n");
    return 1;
}
