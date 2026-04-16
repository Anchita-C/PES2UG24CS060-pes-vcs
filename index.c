#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "object.h"



// 1. KEEP: index_find, index_remove, index_status (The "PROVIDED" code)
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            for (int j = i; j < index->count - 1; j++) {
                index->entries[j] = index->entries[j + 1];
            }
            index->count--;
            return 0;
        }
    }
    return -1;
}

int index_status(const Index *index) {
    for (int i = 0; i < index->count; i++) {
        printf("%s\n", index->entries[i].path);
    }
    return 0;
}


// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // No index yet = empty index, not an error

    char hex[HASH_HEX_SIZE + 2];  // +2 for safety
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        int n = fscanf(f, "%o %64s %llu %u %511s",
                       &e->mode,
                       hex,
                       (unsigned long long *)&e->mtime_sec,
                       &e->size,
                       e->path);

        if (n == EOF || n < 5) break;

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}


// ---------------- FIX ONLY STARTS HERE ----------------

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}


// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // Step 1: Sort a copy of the entries by path
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);

    // Step 2: Write to a temp file in the .pes directory
    char tmp_path[] = ".pes/index_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    for (int i = 0; i < sorted_index.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_index.entries[i].hash, hex);

        fprintf(f, "%o %s %u %u %s\n",
                sorted_index.entries[i].mode,
                hex,
                sorted_index.entries[i].mtime_sec,
                sorted_index.entries[i].size,
                sorted_index.entries[i].path);
    }

    // Step 4: Flush userspace buffer, fsync to disk, close
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // Step 5: Atomically replace the real index file
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// ---------------- FIX ONLY ENDS HERE ----------------

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    // Step 1: Read the file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *contents = malloc((size_t)file_size);
    if (!contents) { fclose(f); return -1; }

    if (file_size > 0 && fread(contents, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(contents); fclose(f); return -1;
    }
    fclose(f);

    // Step 2: Write blob to the object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents);
        fprintf(stderr, "error: failed to store blob for '%s'\n", path);
        return -1;
    }
    free(contents);

    // Step 3: Get file metadata (mode, mtime, size)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (st.st_mode & S_IXUSR) mode = 0100755;
    else                       mode = 0100644;

    // Step 4: Update or insert index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        // Update in place
        existing->hash    = blob_id;
        existing->mode    = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size    = (uint32_t)st.st_size;
    } else {
        // Add new entry
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        IndexEntry *e = &index->entries[index->count++];
        e->hash      = blob_id;
        e->mode      = mode;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    // Step 5: Save the updated index to disk
    return index_save(index);
}