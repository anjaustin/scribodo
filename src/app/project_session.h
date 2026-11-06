#ifndef SCRIBODO_PROJECT_SESSION_H
#define SCRIBODO_PROJECT_SESSION_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

typedef struct {
    char project_dir[PATH_MAX];
    char chat_project[PATH_MAX];
    bool clear_chat_when_idle;
    char chat_id[40];
    char chat_title[160];
    uint64_t generation;
} ProjectSession;

void project_session_init(ProjectSession *session);
bool project_session_set_project(ProjectSession *session, const char *project_dir);
void project_session_clear_chat(ProjectSession *session);
void project_session_bind_chat(ProjectSession *session, const char *project_dir, const char *id, const char *title);
void project_session_destroy(ProjectSession *session);

#endif
