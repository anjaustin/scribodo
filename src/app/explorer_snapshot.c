#define _DARWIN_C_SOURCE
#include "explorer_snapshot.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int compare_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

void explorer_snapshot_init(ExplorerSnapshot *snapshot)
{
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
}

bool explorer_snapshot_build(ExplorerSnapshot *next, const char *project_dir, uint64_t generation,
                             uint64_t project_generation)
{
    if (!next) return false;
    explorer_snapshot_init(next);
    next->generation = generation;
    next->project_generation = project_generation;
    if (!project_dir || !project_dir[0]) return true;
    DIR *dir = opendir(project_dir);
    if (!dir) return false;
    {
        struct dirent *entry;
        bool read_ok = true;
        while (next->ndocs < EXPLORER_MAX_DOCS) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) {
                read_ok = errno == 0;
                break;
            }
            size_t n = strlen(entry->d_name);
            if (entry->d_name[0] == '.') continue;
            bool control = false;
            for (const unsigned char *c = (const unsigned char *)entry->d_name; *c && !control; c++) control = *c < 0x20;
            if (control) continue;
            if (!((n > 3 && !strcasecmp(entry->d_name + n - 3, ".md")) ||
                  (n > 4 && !strcasecmp(entry->d_name + n - 4, ".txt")) ||
                  (n > 9 && !strcasecmp(entry->d_name + n - 9, ".markdown"))))
                continue;
            next->docs[next->ndocs] = strdup(entry->d_name);
            if (!next->docs[next->ndocs]) {
                closedir(dir);
                explorer_snapshot_destroy(next);
                return false;
            }
            next->ndocs++;
        }
        bool ok = closedir(dir) == 0 && read_ok;
        if (!ok) {
            explorer_snapshot_destroy(next);
            return false;
        }
        qsort(next->docs, next->ndocs, sizeof(next->docs[0]), compare_names);
    }
    if (!chat_store_list_checked(project_dir, &next->chats, &next->nchats)) {
        explorer_snapshot_destroy(next);
        return false;
    }
    next->chat_labels = next->nchats ? calloc(next->nchats, sizeof(*next->chat_labels)) : NULL;
    if (next->nchats && !next->chat_labels) {
        explorer_snapshot_destroy(next);
        return false;
    }
    for (size_t i = 0; i < next->nchats; i++)
        snprintf(next->chat_labels[i], sizeof(next->chat_labels[i]), "%s%s", next->chats[i].title,
                 next->chats[i].observer_ignore ? " \xC2\xB7 not observed" : "");
    return true;
}

void explorer_snapshot_destroy(ExplorerSnapshot *snapshot)
{
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->ndocs; i++) free(snapshot->docs[i]);
    free(snapshot->chats);
    free(snapshot->chat_labels);
    memset(snapshot, 0, sizeof(*snapshot));
}

const char *explorer_snapshot_doc(const ExplorerSnapshot *snapshot, uint64_t generation, uint64_t project_generation,
                                  size_t index)
{
    return snapshot && snapshot->generation == generation && snapshot->project_generation == project_generation &&
                   index < snapshot->ndocs
               ? snapshot->docs[index]
               : NULL;
}

const ChatSummary *explorer_snapshot_chat(const ExplorerSnapshot *snapshot, uint64_t generation,
                                          uint64_t project_generation, size_t index)
{
    return snapshot && snapshot->generation == generation && snapshot->project_generation == project_generation &&
                   index < snapshot->nchats
               ? &snapshot->chats[index]
               : NULL;
}
