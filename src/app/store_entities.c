/* Entities and typed links in the project store (SOW §4.3 graph scope,
 * acceptance 7). Built from observer findings: an entity finding names an
 * entity; a relationship finding links two, citing the finding (whose
 * source is the passage). Which findings mention which entity is kept in
 * entity_mentions (migration 004), so counts and lists never depend on
 * re-deriving names in SQL. */
#define _DARWIN_C_SOURCE
#include "store_entities.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/utf8.h"

/* Lowercase of the cased letters a name is likely to use: Latin (ASCII,
 * Latin-1, Latin Extended-A), Greek and Cyrillic. Scripts without case
 * (CJK and others) have no capitals, so names in them aren't recognised
 * (docs: a known limit). */
static uint32_t lower_cp(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0x391 && c <= 0x3A9 && c != 0x3A2)) return c + 32;   /* Greek */
    if (c >= 0x410 && c <= 0x42F) return c + 32;                   /* Cyrillic */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
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

static bool capitalised(const char *w, size_t len)
{
    uint32_t c;
    int n = utf8_decode_one((const uint8_t *)w, len, &c);
    return n > 0 && upper_cp(c);
}

/* A named entity looks like a name: one to four words, each capitalised
 * ("Zoë Ångström", "Ytre Sula"), apart from particles ("Ludwig van
 * Beethoven"). Phrases a model may give as a subject ("Dvořák confronts
 * Zoë", "Zoë's eyes narrowing", "the sea") are not entities. */
static bool a_name(const char *name)
{
    static const char *const particles[] = { "van", "von", "de", "da", "del", "der", "den", "di", "du", "la", "le", "of", "af", "av" };
    int words = 0;
    bool first = true;
    for (const char *p = name; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *w = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - w);
        words++;
        bool particle = false;
        for (size_t k = 0; !first && k < sizeof(particles) / sizeof(particles[0]) && !particle; k++)
            particle = strlen(particles[k]) == len && !strncmp(particles[k], w, len);
        if (!particle && !capitalised(w, len)) return false;
        /* a possessive inside a name means a phrase ("Zoë's eyes") */
        if (*p && len > 2 && (!strncmp(w + len - 2, "'s", 2) || (len > 4 && !strncmp(w + len - 4, "\xE2\x80\x99s", 4))))
            return false;
        first = false;
    }
    return words >= 1 && words <= 4 && strlen(name) >= 2;
}

/* "zoë" for "zoë ångström": the first word of a normalised name. */
static size_t first_word(const char *norm) { return strcspn(norm, " "); }

/* Fold entity `from` into `into` (a first name that turned out to be a
 * full name's): mentions and links move, `from` goes. */
static bool fold_entity(Db *db, const char *from, const char *into)
{
    /* All or nothing (inside the caller's transaction if there is one). */
    bool own = !db_in_transaction(db);
    if (own && !db_begin(db)) return false;
    DbArg a[2] = { DB_S(into), DB_S(from) };
    bool ok = db_run(db, "INSERT IGNORE INTO entity_mentions (entity_id, finding_id) SELECT ?, finding_id FROM entity_mentions WHERE entity_id = ?",
                  a, 2, NULL) &&
           db_run(db, "UPDATE links SET from_entity = ? WHERE from_entity = ?", a, 2, NULL) &&
           db_run(db, "UPDATE links SET to_entity = ? WHERE to_entity = ?", a, 2, NULL) &&
           db_run(db, "DELETE FROM links WHERE from_entity = to_entity AND from_entity = ?", (DbArg[1]){ DB_S(into) }, 1, NULL) &&
           db_run(db, "DELETE FROM entities WHERE id = ?", (DbArg[1]){ DB_S(from) }, 1, NULL);
    if (own) {
        if (ok) ok = db_commit(db);
        else db_rollback(db);
    }
    return ok;
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

static bool id_row(const DbRows *rows, size_t row, const char **id)
{
    return db_rows_has_columns(rows, 1) && db_cell_text(rows, row, 0, id) && strlen(*id) < 40;
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
        const char *id;
        bool valid = r.nrows == 1 && id_row(&r, 0, &id) && strlen(id) < idlen;
        if (valid) snprintf(id_out, idlen, "%s", id);
        db_rows_free(&r);
        return valid;
    }
    db_rows_free(&r);
    /* A first name alone ("Zoë") is the one full name it starts, if there
     * is exactly one ("Zoë Ångström"); two Zoës stay apart. */
    size_t fw = first_word(norm);
    if (!norm[fw]) {
        char like[260];
        snprintf(like, sizeof(like), "%s %%", norm);
        if (!db_query(db, "SELECT id FROM entities WHERE project_id = ? AND norm_name LIKE ? LIMIT 2",
                      (DbArg[2]){ DB_S(pid), DB_S(like) }, 2, &r))
            return false;
        const char *id;
        bool one = r.nrows == 1 && id_row(&r, 0, &id) && strlen(id) < idlen;
        if (one) snprintf(id_out, idlen, "%s", id);
        db_rows_free(&r);
        if (one) return true;
    }
    char id[40], clean[256];
    store_new_id('e', id, sizeof(id));
    display_name(name, clean, sizeof(clean));
    DbArg a[5] = { DB_S(id), DB_S(pid), DB_S(clean), DB_S(norm), DB_S(type && type[0] ? type : "entity") };
    if (!db_run(db, "INSERT IGNORE INTO entities (id, project_id, name, norm_name, type) VALUES (?,?,?,?,?)", a, 5, NULL))
        return false;
    /* Another writer may have created it meanwhile: use whichever is there. */
    if (!db_query(db, "SELECT id FROM entities WHERE project_id = ? AND norm_name = ?", k, 2, &r)) return false;
    const char *stored_id;
    bool ok = r.nrows == 1 && id_row(&r, 0, &stored_id) && strlen(stored_id) < idlen;
    if (ok) snprintf(id_out, idlen, "%s", stored_id);
    db_rows_free(&r);
    /* A new full name absorbs an existing entity for its first name alone,
     * unless another full name starts with it too. */
    if (ok && norm[fw]) {
        char firstn[256], like[260];
        snprintf(firstn, sizeof(firstn), "%.*s", (int)fw, norm);
        snprintf(like, sizeof(like), "%s %%", firstn);
        DbRows f, others;
        if (db_query(db, "SELECT id FROM entities WHERE project_id = ? AND norm_name = ?", (DbArg[2]){ DB_S(pid), DB_S(firstn) }, 2,
                     &f)) {
            const char *from;
            if (f.nrows == 1 && id_row(&f, 0, &from) &&
                db_query(db, "SELECT COUNT(*) FROM entities WHERE project_id = ? AND norm_name LIKE ?",
                          (DbArg[2]){ DB_S(pid), DB_S(like) }, 2, &others)) {
                uint64_t count;
                bool valid = others.nrows == 1 && db_rows_has_columns(&others, 1) &&
                             db_cell_u64(&others, 0, 0, &count);
                if (!valid) ok = false;
                else if (count == 1) ok = fold_entity(db, from, id_out);
                db_rows_free(&others);
            }
            db_rows_free(&f);
        }
    }
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
    const char *id;
    bool ok = r.nrows == 1 && id_row(&r, 0, &id) && !strcmp(id, finding_id);
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
    /* The object may have absorbed the subject ("Zoë" into "Zoë Ångström"):
     * look the subject up again (red-team 009). */
    if (!sdb_entity_upsert(db, pid, f->subject, "entity", a, sizeof(a))) return false;
    if (!strcmp(a, b)) return true;
    if (!mention(db, b, finding_id)) return false;
    /* One link per finding (a unique key: two connections may race). */
    char rel[256];
    utf8_clip(rel, sizeof(rel), f->relation, 255);
    DbArg l[5] = { DB_S(pid), DB_S(a), DB_S(rel), DB_S(b), DB_S(finding_id) };
    /* Only the duplicate is tolerated (INSERT IGNORE would also hide a
     * link to an entity that's gone). */
    return db_run(db,
                  "INSERT INTO links (project_id, from_entity, relation, to_entity, finding_id) VALUES (?,?,?,?,?)"
                  " ON DUPLICATE KEY UPDATE finding_id = finding_id",
                  l, 5, NULL);
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
    bool ok = db_rows_has_columns(&r, 5);
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *id, *type, *subject, *relation, *object;
        ok = db_cell_text(&r, i, 0, &id) && strlen(id) < 40 && db_cell_nullable(&r, i, 1, &type) &&
             db_cell_nullable(&r, i, 2, &subject) && db_cell_nullable(&r, i, 3, &relation) &&
             db_cell_nullable(&r, i, 4, &object);
        if (!ok) break;
        FindingInfo f = { 0 };
        utf8_clip(f.type, sizeof(f.type), type ? type : "", SIZE_MAX);
        utf8_clip(f.subject, sizeof(f.subject), subject ? subject : "", SIZE_MAX);
        utf8_clip(f.relation, sizeof(f.relation), relation ? relation : "", SIZE_MAX);
        utf8_clip(f.object, sizeof(f.object), object ? object : "", SIZE_MAX);
        ok = sdb_entities_from_finding(db, pid, id, &f);
    }
    db_rows_free(&r);
    if (ok) db_run(db, "UPDATE projects SET graph_built = 1 WHERE id = ?", (DbArg[1]){ DB_S(pid) }, 1, NULL);
    return ok;
}

bool sdb_entities_built(Db *db, const char *pid)
{
    DbRows r;
    if (!db_query(db, "SELECT graph_built FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r)) return true; /* don't retry blindly */
    int64_t value;
    bool built = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_i64(&r, 0, 0, &value) && value == 1;
    db_rows_free(&r);
    return built;
}

/* A finding's current status, as a join condition: dismissed findings
 * neither count as mentions nor support links. */
#define LIVE_FINDING(alias) " (SELECT q.current_status FROM records q WHERE q.id = " alias ") NOT IN ('dismissed', 'filtered')"

void entity_list_free(EntityRow *e, size_t n)
{
    (void)n;
    free(e);
}

size_t sdb_entity_list(Db *db, const char *pid, EntityRow **out)
{
    *out = NULL;
    DbRows r;
    /* Mentions and links of live (not dismissed) findings, by the
     * findings' current status (indexed). */
    if (!db_query(db,
                  "SELECT e.id, e.name, COALESCE(mc.n, 0), COALESCE(lc.n, 0) FROM entities e"
                  " LEFT JOIN (SELECT m.entity_id, COUNT(*) AS n FROM entity_mentions m JOIN records r ON r.id = m.finding_id"
                  "            WHERE r.project_id = ? AND r.current_status NOT IN ('dismissed', 'filtered') GROUP BY m.entity_id) mc ON mc.entity_id = e.id"
                  " LEFT JOIN (SELECT x.eid, COUNT(*) AS n FROM"
                  "             (SELECT l.from_entity AS eid FROM links l JOIN records r ON r.id = l.finding_id"
                  "              WHERE l.project_id = ? AND r.current_status NOT IN ('dismissed', 'filtered')"
                  "              UNION ALL SELECT l.to_entity FROM links l JOIN records r ON r.id = l.finding_id"
                  "              WHERE l.project_id = ? AND r.current_status NOT IN ('dismissed', 'filtered')) x"
                  "            GROUP BY x.eid) lc ON lc.eid = e.id"
                  " WHERE e.project_id = ? AND COALESCE(mc.n, 0) + COALESCE(lc.n, 0) > 0"
                  " ORDER BY COALESCE(lc.n, 0) + COALESCE(mc.n, 0) DESC, e.name",
                  (DbArg[4]){ DB_S(pid), DB_S(pid), DB_S(pid), DB_S(pid) }, 4, &r))
        return 0;
    bool ok = db_rows_has_columns(&r, 4);
    EntityRow *list = ok && r.nrows ? calloc(r.nrows, sizeof(EntityRow)) : NULL;
    if (r.nrows && !list) ok = false;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *id, *name;
        int64_t mentions, links;
        ok = db_cell_text(&r, i, 0, &id) && strlen(id) < sizeof(list[i].id) &&
             db_cell_text(&r, i, 1, &name) && db_cell_i64(&r, i, 2, &mentions) && mentions >= 0 && mentions <= INT_MAX &&
             db_cell_i64(&r, i, 3, &links) && links >= 0 && links <= INT_MAX;
        if (!ok) break;
        snprintf(list[i].id, sizeof(list[i].id), "%s", id);
        utf8_clip(list[i].name, sizeof(list[i].name), name, SIZE_MAX);
        list[i].mentions = (int)mentions;
        list[i].links = (int)links;
    }
    size_t n = ok ? r.nrows : 0;
    db_rows_free(&r);
    if (!ok) {
        free(list);
        list = NULL;
    }
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
    const char *entity_name;
    if (e.nrows != 1 || !db_rows_has_columns(&e, 1) || !db_cell_text(&e, 0, 0, &entity_name)) {
        db_rows_free(&e);
        return false;
    }
    snprintf(d->id, sizeof(d->id), "%s", entity_id);
    utf8_clip(d->name, sizeof(d->name), entity_name, SIZE_MAX);
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
    bool ok = db_rows_has_columns(&l, 6) && db_rows_has_columns(&m, 2) && l.nrows <= SIZE_MAX - m.nrows;
    size_t n = ok ? l.nrows + m.nrows : 0;
    d->rows = ok && n ? calloc(n, sizeof(EntityDetailRow)) : NULL;
    if (n && !d->rows) ok = false;
    for (size_t i = 0; ok && i < l.nrows; i++) {
        const char *relation, *finding, *from_id, *from_name, *to_id, *to_name;
        ok = db_cell_text(&l, i, 0, &relation) && db_cell_text(&l, i, 1, &finding) &&
             db_cell_text(&l, i, 2, &from_id) && db_cell_text(&l, i, 3, &from_name) &&
             db_cell_text(&l, i, 4, &to_id) && db_cell_text(&l, i, 5, &to_name) &&
             strlen(finding) < sizeof(((EntityDetailRow *)0)->finding_id) &&
             strlen(from_id) < sizeof(((EntityDetailRow *)0)->other_id) &&
             strlen(to_id) < sizeof(((EntityDetailRow *)0)->other_id);
        if (!ok) break;
        EntityDetailRow *row = &d->rows[d->nrows++];
        bool outgoing = !strcmp(from_id, entity_id);
        row->is_link = true;
        row->outgoing = outgoing;
        utf8_clip(row->relation, sizeof(row->relation), relation, SIZE_MAX);
        snprintf(row->finding_id, sizeof(row->finding_id), "%s", finding);
        snprintf(row->other_id, sizeof(row->other_id), "%s", outgoing ? to_id : from_id);
        utf8_clip(row->other_name, sizeof(row->other_name), outgoing ? to_name : from_name, SIZE_MAX);
    }
    for (size_t i = 0; ok && i < m.nrows; i++) {
        const char *finding, *text;
        ok = db_cell_text(&m, i, 0, &finding) && strlen(finding) < sizeof(((EntityDetailRow *)0)->finding_id) &&
             db_cell_text(&m, i, 1, &text);
        if (!ok) break;
        bool linked = false; /* shown above as its link */
        for (size_t k = 0; ok && k < l.nrows && !linked; k++) {
            const char *link_finding;
            ok = db_cell_text(&l, k, 1, &link_finding);
            linked = ok && !strcmp(link_finding, finding);
        }
        if (!ok) break;
        if (linked) continue;
        EntityDetailRow *row = &d->rows[d->nrows++];
        snprintf(row->finding_id, sizeof(row->finding_id), "%s", finding);
        utf8_clip(row->text, sizeof(row->text), text, SIZE_MAX);
    }
    db_rows_free(&l);
    db_rows_free(&m);
    if (!ok) entity_detail_free(d);
    return ok;
}
