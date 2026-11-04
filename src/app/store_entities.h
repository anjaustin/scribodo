/* Entities and typed links (SOW §4.3 graph scope, acceptance 7).
 *
 * Built from observer findings in the project store: an entity finding
 * names an entity; a relationship finding links two named entities and is
 * cited as the link's evidence (its source is the passage). Entities are
 * matched by a normalised name (case, possessive and punctuation ignored).
 * Store-only: projects on files have no graph.
 */
#ifndef SCRIBODO_STORE_ENTITIES_H
#define SCRIBODO_STORE_ENTITIES_H

#include <stdbool.h>
#include <stddef.h>

#include "app/db.h"
#include "app/records.h"
#include "app/store.h"

/* "Zoë Ångström's" -> "zoë ångström". */
void entity_norm(const char *name, char *out, size_t outlen);

/* The entity with this (normalised) name, created if new. False if the
 * name isn't a named entity (no capital) or on error. */
bool sdb_entity_upsert(Db *db, const char *pid, const char *name, const char *type, char *id_out, size_t idlen);

/* Record what a finding says about entities (idempotent: one link per
 * finding). False only on a database error. */
bool sdb_entities_from_finding(Db *db, const char *pid, const char *finding_id, const FindingInfo *f);

/* Rebuild entities and links from every finding of the project. */
bool sdb_entities_backfill(Db *db, const char *pid);

typedef struct {
    char id[40];
    char name[256];
    int mentions; /* findings about it (not dismissed) */
    int links;
} EntityRow;

/* Entities, most connected first. */
size_t sdb_entity_list(Db *db, const char *pid, EntityRow **out);
void entity_list_free(EntityRow *e, size_t n);

typedef struct {
    bool is_link;
    bool outgoing;       /* link: this entity is the subject */
    char relation[256];  /* link */
    char other_id[40], other_name[256];
    char finding_id[40]; /* the supporting finding (link) or the mention */
    char text[512];      /* mention: the finding's text */
} EntityDetailRow;

typedef struct {
    char id[40];
    char name[256];
    EntityDetailRow *rows; /* links first, then mentions (newest first) */
    size_t nrows;
} EntityDetail;

bool sdb_entity_detail(Db *db, const char *pid, const char *entity_id, EntityDetail *d);
void entity_detail_free(EntityDetail *d);

#endif
