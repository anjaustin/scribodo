/* Who is who (user testing 2026-11-04): the author's name and the name
 * the author gives the assistant, as stored facts. They go into every chat
 * request as facts, label the chat, and keep both people out of observer
 * findings and the entity graph, so nothing depends on a model inferring
 * who is who. App-wide: ~/Library/Application Support/Scribodo/profile.json
 * (or $SCRIBODO_PROFILE). */
#ifndef SCRIBODO_PROFILE_H
#define SCRIBODO_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char author[64];    /* "" = not set */
    char assistant[64]; /* "" = not set (the app calls it "Scribodo") */
    bool asked;         /* the author has been asked once (first launch) */
} Profile;

/* Missing or unreadable: an empty profile (asked = false). */
void profile_load(Profile *p);
bool profile_save(const Profile *p);

/* A name as stored: trimmed, control characters and quotes removed,
 * at most 60 bytes on a character boundary. "" if nothing is left. */
void profile_clean_name(const char *in, char *out, size_t outlen);

/* The assistant's display name ("Scribodo" when not set). */
const char *profile_assistant_name(const Profile *p);

#endif
