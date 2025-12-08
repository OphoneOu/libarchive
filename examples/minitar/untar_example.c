#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
copy_data(struct archive *ar, struct archive *aw)
{
    const void *buff;
    size_t size;
    la_int64_t offset;
    int r;

    while ((r = archive_read_data_block(ar, &buff, &size, &offset)) == ARCHIVE_OK) {
        r = archive_write_data_block(aw, buff, size, offset);
        if (r != ARCHIVE_OK) {
            fprintf(stderr, "write_data_block: %s\n", archive_error_string(aw));
            return r;
        }
    }
    if (r != ARCHIVE_EOF) {
        fprintf(stderr, "read_data_block: %s\n", archive_error_string(ar));
    }
    return r == ARCHIVE_EOF ? ARCHIVE_OK : r;
}

static int
append_destination_prefix(struct archive_entry *entry, const char *destdir)
{
    const char *path = archive_entry_pathname(entry);
    if (path == NULL)
        path = "";

    size_t need_slash = destdir[0] && destdir[strlen(destdir) - 1] == '/' ? 0 : 1;
    size_t len = strlen(destdir) + need_slash + strlen(path) + 1;
    char *full = malloc(len);
    if (full == NULL) {
        fprintf(stderr, "malloc failed: %s\n", strerror(errno));
        return ARCHIVE_FATAL;
    }
    strcpy(full, destdir);
    if (need_slash)
        strcat(full, "/");
    strcat(full, path);

    archive_entry_set_pathname(entry, full);
    free(full);
    return ARCHIVE_OK;
}

static int
extract_tar(const char *filename, const char *destdir)
{
    struct archive *a = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    struct archive_entry *entry;
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;
    int r = ARCHIVE_OK;

    archive_read_support_filter_none(a);
    archive_read_support_format_tar(a);

    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);

    if ((r = archive_read_open_filename(a, filename, 10240))) {
        fprintf(stderr, "open: %s\n", archive_error_string(a));
        goto cleanup;
    }

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (append_destination_prefix(entry, destdir) != ARCHIVE_OK) {
            r = ARCHIVE_FATAL;
            break;
        }

        r = archive_write_header(ext, entry);
        if (r != ARCHIVE_OK) {
            fprintf(stderr, "write_header: %s\n", archive_error_string(ext));
        } else {
            if (archive_entry_size(entry) > 0) {
                r = copy_data(a, ext);
                if (r != ARCHIVE_OK)
                    break;
            }
        }

        r = archive_write_finish_entry(ext);
        if (r != ARCHIVE_OK) {
            fprintf(stderr, "finish_entry: %s\n", archive_error_string(ext));
            break;
        }
    }

    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
        fprintf(stderr, "next_header: %s\n", archive_error_string(a));
    } else {
        r = ARCHIVE_OK;
    }

cleanup:
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return r;
}

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <archive.tar> <destination-dir>\n", argv[0]);
        return 1;
    }

    if (extract_tar(argv[1], argv[2]) != ARCHIVE_OK) {
        fprintf(stderr, "Extraction failed\n");
        return 1;
    }
    return 0;
}
