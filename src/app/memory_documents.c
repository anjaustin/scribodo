#define _DARWIN_C_SOURCE
#include "memory_documents.h"
#include "app/memory_saved_file.h"
#include "app/store_backends.h"

#include <stdio.h>
#include <string.h>

bool memory_document_name_copy(const char *title, bool dirty, MemoryDocumentName *out)
{
    if (!out) return false;
    *out = (MemoryDocumentName){0};
    if (dirty || !title || !title[0] || strnlen(title, sizeof(out->name)) >= sizeof(out->name)) return false;
    memcpy(out->name, title, strlen(title) + 1);
    return true;
}

static bool identity_current(StoreManager *store, const char *path, const StoreProjectIdentity *expected)
{
    StoreProjectIdentity now;
    return expected && store_project_identity(store, path, &now) && now.generation == expected->generation &&
           now.device == expected->device && now.inode == expected->inode && !strcmp(now.project_id, expected->project_id);
}

static bool reference_same(const MemoryDocumentRef *a, const MemoryDocumentRef *b)
{
    return !strcmp(a->database, b->database) && !strcmp(a->project_id, b->project_id) &&
           !strcmp(a->document_id, b->document_id) && !strcmp(a->revision_id, b->revision_id) &&
           !strcmp(a->source_hash, b->source_hash) && !strcmp(a->name, b->name) && a->bytes == b->bytes;
}

static bool reference_valid(const MemoryDocumentRef *ref)
{
    const char *fields[] = { ref->database, ref->project_id, ref->document_id, ref->revision_id, ref->name };
    const size_t sizes[] = { sizeof(ref->database), sizeof(ref->project_id), sizeof(ref->document_id),
                            sizeof(ref->revision_id), sizeof(ref->name) };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        if (!fields[i][0] || strnlen(fields[i], sizes[i]) == sizes[i]) return false;
    return strnlen(ref->source_hash, sizeof(ref->source_hash)) == 64 &&
           strspn(ref->source_hash, "0123456789abcdef") == 64 && ref->bytes <= MEMORY_SAVED_FILE_MAX_BYTES;
}

bool memory_documents_same(const MemoryDocumentSet *a, const MemoryDocumentSet *b)
{
    if (!a || !b || a->count > MEMORY_DOCUMENTS_MAX || a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (!reference_valid(&a->refs[i]) || !reference_valid(&b->refs[i]) || !reference_same(&a->refs[i], &b->refs[i])) return false;
    return true;
}

static MemoryDocumentRef from_saved(const SavedRevisionIdentity *saved, const char *name)
{
    MemoryDocumentRef ref = { .bytes = saved->bytes };
    snprintf(ref.database, sizeof(ref.database), "%s", saved->database);
    snprintf(ref.project_id, sizeof(ref.project_id), "%s", saved->project_id);
    snprintf(ref.document_id, sizeof(ref.document_id), "%s", saved->document_id);
    snprintf(ref.revision_id, sizeof(ref.revision_id), "%s", saved->revision_id);
    snprintf(ref.source_hash, sizeof(ref.source_hash), "%s", saved->source_hash);
    snprintf(ref.name, sizeof(ref.name), "%s", name);
    return ref;
}

bool memory_documents_capture(StoreManager *store, const char *path,
                               const StoreProjectIdentity *project, const MemoryDocumentName *names,
                               size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out)
{
    if (!out) return false;
    *out = (MemoryDocumentSet){0};
    if (remote) return true;
    if (!store || !path || !project || count > MEMORY_DOCUMENTS_MAX || (count && !names) ||
        (cancel && atomic_load(cancel)) || !identity_current(store, path, project)) return false;
    char id[64]; Db *db = NULL;
    if (store_mode(store, path, id, sizeof(id), &db) != STORE_DB || !db || strcmp(id, project->project_id)) return false;
    MemoryDocumentSet result = {0};
    for (size_t i = 0; i < count; i++) {
        if (cancel && atomic_load(cancel)) return false;
        if (names[i].dirty || !names[i].name[0] || strnlen(names[i].name, sizeof(names[i].name)) == sizeof(names[i].name)) continue;
        SavedRevisionIdentity saved;
        if (!sdb_saved_revision_identity(db, id, names[i].name, &saved)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < result.count; j++)
            if (!strcmp(saved.document_id, result.refs[j].document_id)) { duplicate = true; break; }
        if (duplicate) continue;
        if (memory_saved_file_matches(path, names[i].name, project, saved.source_hash, saved.bytes, false, false, cancel))
            result.refs[result.count++] = from_saved(&saved, names[i].name);
    }
    if ((cancel && atomic_load(cancel)) || !identity_current(store, path, project)) return false;
    *out = result;
    return true;
}

bool memory_documents_admit(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents)
{
    if (!db || db_in_transaction(db) || !scope || !scope->enabled || !documents || documents->count > MEMORY_DOCUMENTS_MAX) return false;
    for (size_t i = 0; i < documents->count; i++) {
        const MemoryDocumentRef *ref = &documents->refs[i];
        if (!reference_valid(ref) || strnlen(ref->name, sizeof(ref->name)) == sizeof(ref->name) ||
            strnlen(ref->database, sizeof(ref->database)) == sizeof(ref->database) ||
            strnlen(ref->project_id, sizeof(ref->project_id)) == sizeof(ref->project_id) ||
            strnlen(ref->document_id, sizeof(ref->document_id)) == sizeof(ref->document_id) ||
            strnlen(ref->revision_id, sizeof(ref->revision_id)) == sizeof(ref->revision_id) ||
            strnlen(ref->source_hash, sizeof(ref->source_hash)) == sizeof(ref->source_hash) ||
            strcmp(ref->project_id, scope->project_id) || strcmp(ref->database, scope->database)) return false;
        SavedRevisionIdentity saved;
        if (!sdb_saved_revision_identity(db, scope->project_id, ref->name, &saved)) return false;
        MemoryDocumentRef now = from_saved(&saved, ref->name);
        if (!reference_same(ref, &now)) return false;
    }
    return true;
}
