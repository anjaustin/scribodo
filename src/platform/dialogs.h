/* Native file dialogs and Finder integration (macOS: NSOpenPanel,
 * NSWorkspace). Under a UI script no panel opens: each dialog takes the next
 * answer queued with dialogs_script_answer() ("" means Cancel), so scripts
 * stay headless and deterministic.
 */
#ifndef SCRIBODO_DIALOGS_H
#define SCRIBODO_DIALOGS_H

#include <stdbool.h>
#include <stddef.h>

/* Choose a folder (optionally allowing "New Folder"). False on Cancel. */
bool dialogs_choose_folder(const char *message, const char *prompt, bool allow_create, char *out, size_t outlen);
/* Choose one or more .md/.txt files. Returns how many; *paths is a malloc'd
 * array of malloc'd paths (free with dialogs_free_paths). 0 on Cancel. */
size_t dialogs_choose_files(const char *message, char ***paths);
void dialogs_free_paths(char **paths, size_t n);
/* Show a file or folder in Finder. */
void dialogs_reveal(const char *path);

/* UI scripts: enable scripted answers, and queue the next one. A file answer
 * may hold several paths separated by '|'. */
void dialogs_script_mode(bool on);
void dialogs_script_answer(const char *answer);

#endif
