#include "project_session.h"

#include <stdio.h>
#include <string.h>

void project_session_init(ProjectSession *session)
{
    if (session) memset(session, 0, sizeof(*session));
}

bool project_session_set_project(ProjectSession *session, const char *project_dir)
{
    if (!session || !project_dir || !strcmp(session->project_dir, project_dir)) return false;
    snprintf(session->project_dir, sizeof(session->project_dir), "%s", project_dir);
    session->generation++;
    return true;
}

void project_session_clear_chat(ProjectSession *session)
{
    if (!session) return;
    session->chat_project[0] = '\0';
    session->chat_id[0] = '\0';
    session->chat_title[0] = '\0';
}

void project_session_bind_chat(ProjectSession *session, const char *project_dir, const char *id, const char *title)
{
    if (!session) return;
    char project_copy[PATH_MAX] = { 0 }, id_copy[40] = { 0 }, title_copy[160] = { 0 };
    snprintf(project_copy, sizeof(project_copy), "%s", project_dir ? project_dir : "");
    snprintf(id_copy, sizeof(id_copy), "%s", id ? id : "");
    snprintf(title_copy, sizeof(title_copy), "%s", title ? title : "");
    memcpy(session->chat_project, project_copy, sizeof(project_copy));
    memcpy(session->chat_id, id_copy, sizeof(id_copy));
    memcpy(session->chat_title, title_copy, sizeof(title_copy));
}

void project_session_destroy(ProjectSession *session)
{
    if (session) memset(session, 0, sizeof(*session));
}
