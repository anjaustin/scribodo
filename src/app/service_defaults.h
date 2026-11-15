/* The local services' defaults for the C side: the same values as tools/services.defaults, the file the scripts read.
 * tests/test_service_defaults.py compiles this header and fails if any value differs from that file. */
#ifndef SCRIBODO_SERVICE_DEFAULTS_H
#define SCRIBODO_SERVICE_DEFAULTS_H

#define SCRIBODO_DEFAULT_DB_PORT 33061
#define SCRIBODO_DEFAULT_QDRANT_PORT 6333
/* The secrets folder under $HOME, and its tail (which also names it when the folder is spelled another way). */
#define SCRIBODO_SERVICES_DIR_TAIL "/Scribodo/services"
#define SCRIBODO_DEFAULT_SECRETS_DIR "Library/Application Support" SCRIBODO_SERVICES_DIR_TAIL

#endif
