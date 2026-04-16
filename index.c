#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>


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

// 2. REPLACE: The TODO section with this:

// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    char path[MAX_PATH_LEN];
    uint32_t mode;
    long mtime;
    size_t size;

    while (fscanf(f, "%o %64s %ld %zu %[^\n]", &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *e = &index->entries[index->count++];
        e->mode = mode;
        e->mtime_sec = (uint32_t)mtime;
        e->size = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        hex_to_hash(hex, &e->hash);
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);

    char tmp_path[] = ".pes/index_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;
    
    FILE *f = fdopen(fd, "w");
    for (int i = 0; i < sorted_index.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_index.entries[i].hash, hex);
        fprintf(f, "%o %s %u %u %s\n", 
                sorted_index.entries[i].mode, hex, 
                sorted_index.entries[i].mtime_sec, 
                sorted_index.entries[i].size, 
                sorted_index.entries[i].path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, ".pes/index") != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *data = malloc(st.st_size);
    fread(data, 1, st.st_size, f);
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path) - 1);
    }

    e->mode = get_file_mode(path);
    e->hash = id;
    e->mtime_sec = (uint32_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;

    return index_save(index);
}