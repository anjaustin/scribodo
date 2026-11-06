/* Transition coordination shared by the one-shot router and explicit exports. */
#ifndef SCRIBODO_PROJECT_STORE_INTERNAL_H
#define SCRIBODO_PROJECT_STORE_INTERNAL_H

int project_store_file_lock_shared(const char *project_dir);
void project_store_file_unlock(int fd);

#endif
