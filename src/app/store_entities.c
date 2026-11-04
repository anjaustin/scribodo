/* Entities and typed links in the project store (SOW §4.3 graph scope,
 * acceptance 7). Built from observer findings: an entity finding names an
 * entity; a relationship finding links two, citing the finding (whose
 * source is the passage). Which findings mention which entity is kept in
 * entity_mentions (migration 004), so counts and lists never depend on
 * re-deriving names in SQL. */
#define _DARWIN_C_SOURCE
#include "store_entities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/utf8.h"

/* Lowercase of the Latin letters a name is likely to use (ASCII, Latin-1,
 * Latin Extended-A); other letters are kept as they are. */
static uint32_t lower_cp(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    return c;
}

static bool upper_cp(uint32_t c) { return lower_cp(c) != c || (c >= 'A' && c <= 'Z'); }

/* "Zoë Ångström's" -> "zoë ångström": lowercased, possessive and
 * surrounding punctuation dropped, spaces collapsed. Whole characters
 * only (never cut in two). */
void entity_norm(const char *name, char *out, size_t outlen)
{
    size_t k = 0, len = strlen(name);
    bool space = false;
    if (!outlen) return;
    for (size_t i = 0; i < len;) {
        uint32_t c;
        int n = utf8_decode_one((const uint8_t *)name + i, len - i, &c);
        if (n <= 0) {
            i++;
            continue;
        }
        i += (size_t)n;
        if (c == ' ' || c == '\t' || c == '\n' || c == 0xA0) {
            space = k > 0;
            continue;
        }
        if (c == 0x2019) c = '\''; /* ’ */
        if (c < 0x80 && !((c | 0x20) >= 'a' && (c | 0x20) <= 'z') && !(c >= '0' && c <= '9') && c != '\'' && c != '-') continue;
        uint8_t enc[4];
        int m = utf8_encode_one(lower_cp(c), enc);
        if (k + (space ? 1 : 0) + (size_t)m + 1 > outlen) break;
        if (space) out[k++] = ' ';
        space = false;
        memcpy(out + k, enc, (size_t)m);
        k += (size_t)m;
    }
    out[k] = '\0';
    if (k > 2 && out[k - 1] == 's' && out[k - 2] == '\'') out[k -= 2] = '\0'; /* possessive */
    while (k > 0 && (out[k - 1] == '\'' || out[k - 1] == '-')) out[--k] = '\0';
    size_t s = 0;
    while (out[s] == '\'' || out[s] == '-') s++;
    if (s) memmove(out, out + s, k - s + 1);
}

/* A named entity: starts with a capital letter, two characters or more. */
static bool a_name(const char *name)
{
    while (*name == ' ') name++;
    uint32_t c;
    int n = utf8_decode_one((const uint8_t *)name, strlen(name), &c);
    return n > 0 && upper_cp(c) && strlen(name) >= 2;
}

/* The display name: trimmed, possessive dropped, at most 255 bytes. */
static void display_name(const char *name, char *out, size_t outlen)
{
    while (*name == ' ') name++;
    utf8_clip(out, outlen < 256 ? outlen : 256, name, 255);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.' || out[n - 1] == ',')) out[--n] = '\0';
    if (n > 2 && out[n - 1] == 's' && out[n - 2] == '\'') out[n - 2] = '\0';
    else if (n > 4 && !strcmp(out + n - 4, "\xE2\x80\x99s")) out[n - 4] = '\0';
}

bool sdb_entity_upsert(Db *db, const char *pid, const char *name, const char *type, char *id_out, size_t idlen)
{
    char norm[256];
    entity_norm(name, norm, sizeof(norm));
    if (!norm[0] || !a_name(name)) return false;
    DbRows r;
    DbArg k[2] = { DB_S(pid), DB_S(norm) };
    if (!db_query(db, "SELECT id FROM entities WHERE project_id = ? AND norm_name = ?", k, 2, &r)) return false;
    if (r.nrows) {
        snprintf(id_out, idlen, "%s", db_cell(&r, 0, 0));
        db_rows_free(&r);
        return true;
    }
    db_rows_free(&r);
    char id[40], clean[256];
    store_new_id('e', id, sizeof(id));
    display_name(name, clean, sizeof(clean));
    DbArg a[5] = { DB_S(id), DB_S(pid), DB_S(clean), DB_S(norm), DB_S(type && type[0] ? type : "entity") };
    if (!db_run(db, "INSERT IGNORE INTO entities (id, project_id, name, norm_name, type) VALUES (?,?,?,?,?)", a, 5, NULL))
        return false;
    /* Another writer may have created it meanwhile: use whichever is there. */
    if (!db_query(db, "SELECT id FROM entities WHERE project_id = ? AND norm_name = ?", k, 2, &r)) return false;
    bool ok = r.nrows > 0;
    if (ok) snprintf(id_out, idlen, "%s", db_cell(&r, 0, 0));
    db_rows_free(&r);
    return ok;
}

static bool mention(Db *db, const char *entity_id, const char *finding_id)
{
    return db_run(db, "INSERT IGNORE INTO entity_mentions (entity_id, finding_id) VALUES (?,?)",
                  (DbArg[2]){ DB_S(entity_id), DB_S(finding_id) }, 2, NULL);
}

/* The finding must be this project's (never link another project's). */
static bool own_finding(Db *db, const char *pid, const char *finding_id)
{
    DbRows r;
    if (!db_query(db, "SELECT id FROM records WHERE id = ? AND project_id = ? AND kind = 'finding'",
                  (DbArg[2]){ DB_S(finding_id), DB_S(pid) }, 2, &r))
        return false;
    bool ok = r.nrows == 1;
    db_rows_free(&r);
    return ok;
}

bool sdb_entities_from_finding(Db *db, const char *pid, const char *finding_id, const FindingInfo *f)
{
    if (!f || !f->type[0] || !f->subject[0] || !a_name(f->subject)) return true;
    if (!own_finding(db, pid, finding_id)) return true;
    char a[40], b[40];
    if (!sdb_entity_upsert(db, pid, f->subject, "entity", a, sizeof(a))) return false;
    if (!mention(db, a, finding_id)) return false;
    if (strcmp(f->type, "relationship") != 0) return true; /* entity, event, question, conflict: a mention */
    if (!f->object[0] || !f->relation[0] || !a_name(f->object)) return true; /* unnamed ends are not linked */
    if (!sdb_entity_upsert(db, pid, f->object, "entity", b, sizeof(b))) return false;
    if (!strcmp(a, b)) return true;
    if (!mention(db, b, finding_id)) return false;
    /* One link per finding. */
    DbRows r;
    if (!db_query(db, "SELECT id FROM links WHERE finding_id = ?", (DbArg[1]){ DB_S(finding_id) }, 1, &r)) return false;
    bool have = r.nrows > 0;
    db_rows_free(&r);
    if (have) return true;
    char rel[256];
    utf8_clip(rel, sizeof(rel), f->relation, 255);
    DbArg l[5] = { DB_S(pid), DB_S(a), DB_S(rel), DB_S(b), DB_S(finding_id) };
    return db_run(db, "INSERT INTO links (project_id, from_entity, relation, to_entity, finding_id) VALUES (?,?,?,?,?)", l, 5,
                  NULL);
}

bool sdb_entities_backfill(Db *db, const char *pid)
{
    DbRows r;
    /* Every finding's first version (its type and subject never change). */
    if (!db_query(db,
                  "SELECT r.id, v.finding_type, v.subject, v.relation, v.object FROM records r"
                  " JOIN record_versions v ON v.record_id = r.id AND v.ver = 1"
                  " WHERE r.project_id = ? AND r.kind = 'finding' ORDER BY r.created_at",
                  (DbArg[1]){ DB_S(pid) }, 1, &r))
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        FindingInfo f = { 0 };
#define CP(dst, col) utf8_clip(f.dst, sizeof(f.dst), db_cell(&r, i, col) ? db_cell(&r, i, col) : "", SIZE_MAX)
        CP(type, 1);
        CP(subject, 2);
        CP(relation, 3);
        CP(object, 4);
#undef CP
        ok = sdb_entities_from_finding(db, pid, db_cell(&r, i, 0), &f);
    }
    db_rows_free(&r);
    return ok;
}

/* A finding's current status, as a join condition: dismissed findings
 * neither count as mentions nor support links. */
#define LIVE_FINDING(alias)                                                                                         \
    " (SELECT v.status FROM record_versions v WHERE v.record_id = " alias " ORDER BY v.ver DESC LIMIT 1)"     \
    " <> 'dismissed'"

void entity_list_free(EntityRow *e, size_t n)
{
    (void)n;
    free(e);
}

size_t sdb_entity_list(Db *db, const char *pid, EntityRow **out)
{
    *out = NULL;
    DbRows r;
    if (!db_query(db,
                  "SELECT e.id, e.name,"
                  " (SELECT COUNT(*) FROM entity_mentions m WHERE m.entity_id = e.id AND" LIVE_FINDING("m.finding_id") ") AS mentions,"
                  " (SELECT COUNT(*) FROM links l WHERE (l.from_entity = e.id OR l.to_entity = e.id) AND l.finding_id IS NOT NULL"
                  "   AND" LIVE_FINDING("l.finding_id") ") AS nlinks"
                  " FROM entities e WHERE e.project_id = ?"
                  " HAVING mentions + nlinks > 0 ORDER BY nlinks + mentions DESC, e.name",
                  (DbArg[1]){ DB_S(pid) }, 1, &r))
        return 0;
    EntityRow *list = r.nrows ? calloc(r.nrows, sizeof(EntityRow)) : NULL;
    for (size_t i = 0; list && i < r.nrows; i++) {
        snprintf(list[i].id, sizeof(list[i].id), "%s", db_cell(&r, i, 0));
        utf8_clip(list[i].name, sizeof(list[i].name), db_cell(&r, i, 1) ? db_cell(&r, i, 1) : "", SIZE_MAX);
        list[i].mentions = atoi(db_cell(&r, i, 2) ? db_cell(&r, i, 2) : "0");
        list[i].links = atoi(db_cell(&r, i, 3) ? db_cell(&r, i, 3) : "0");
    }
    size_t n = list ? r.nrows : 0;
    db_rows_free(&r);
    *out = list;
    return n;
}

void entity_detail_free(EntityDetail *d)
{
    free(d->rows);
    memset(d, 0, sizeof(*d));
}

bool sdb_entity_detail(Db *db, const char *pid, const char *entity_id, EntityDetail *d)
{
    memset(d, 0, sizeof(*d));
    DbRows e, l, m;
    if (!db_query(db, "SELECT name FROM entities WHERE id = ? AND project_id = ?", (DbArg[2]){ DB_S(entity_id), DB_S(pid) }, 2,
                  &e))
        return false;
    if (!e.nrows) {
        db_rows_free(&e);
        return false;
    }
    snprintf(d->id, sizeof(d->id), "%s", entity_id);
    utf8_clip(d->name, sizeof(d->name), db_cell(&e, 0, 0) ? db_cell(&e, 0, 0) : "", SIZE_MAX);
    db_rows_free(&e);
    /* Relationships in both directions, with the other end and the finding
     * that supports each. */
    if (!db_query(db,
                  "SELECT l.relation, l.finding_id, a.id, a.name, b.id, b.name FROM links l"
                  " JOIN entities a ON a.id = l.from_entity JOIN entities b ON b.id = l.to_entity"
                  " WHERE l.project_id = ? AND (l.from_entity = ? OR l.to_entity = ?) AND l.finding_id IS NOT NULL"
                  " AND" LIVE_FINDING("l.finding_id") " ORDER BY l.id",
                  (DbArg[3]){ DB_S(pid), DB_S(entity_id), DB_S(entity_id) }, 3, &l))
        return false;
    /* Mentions other than relationships (shown as links), newest first. */
    if (!db_query(db,
                  "SELECT r.id, (SELECT v.content FROM record_versions v WHERE v.record_id = r.id ORDER BY v.ver DESC LIMIT 1)"
                  " FROM entity_mentions em JOIN records r ON r.id = em.finding_id"
                  " WHERE em.entity_id = ? AND r.project_id = ? AND" LIVE_FINDING("r.id")
                  " ORDER BY r.created_at DESC LIMIT 40",
                  (DbArg[2]){ DB_S(entity_id), DB_S(pid) }, 2, &m)) {
        db_rows_free(&l);
        return false;
    }
    size_t n = l.nrows + m.nrows;
    d->rows = n ? calloc(n, sizeof(EntityDetailRow)) : NULL;
    for (size_t i = 0; d->rows && i < l.nrows; i++) {
        EntityDetailRow *row = &d->rows[d->nrows++];
        bool outgoing = !strcmp(db_cell(&l, i, 2), entity_id);
        row->is_link = true;
        row->outgoing = outgoing;
        utf8_clip(row->relation, sizeof(row->relation), db_cell(&l, i, 0) ? db_cell(&l, i, 0) : "", SIZE_MAX);
        snprintf(row->finding_id, sizeof(row->finding_id), "%s", db_cell(&l, i, 1) ? db_cell(&l, i, 1) : "");
        snprintf(row->other_id, sizeof(row->other_id), "%s", db_cell(&l, i, outgoing ? 4 : 2));
        utf8_clip(row->other_name, sizeof(row->other_name), db_cell(&l, i, outgoing ? 5 : 3), SIZE_MAX);
    }
    for (size_t i = 0; d->rows && i < m.nrows; i++) {
        bool linked = false; /* shown above as its link */
        for (size_t k = 0; k < l.nrows && !linked; k++) linked = db_cell(&l, k, 1) && !strcmp(db_cell(&l, k, 1), db_cell(&m, i, 0));
        if (linked) continue;
        EntityDetailRow *row = &d->rows[d->nrows++];
        snprintf(row->finding_id, sizeof(row->finding_id), "%s", db_cell(&m, i, 0));
        utf8_clip(row->text, sizeof(row->text), db_cell(&m, i, 1) ? db_cell(&m, i, 1) : "", SIZE_MAX);
    }
    db_rows_free(&l);
    db_rows_free(&m);
    return true;
}
