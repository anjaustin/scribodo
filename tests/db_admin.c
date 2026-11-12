/* Throwaway test databases: created as root and granted to the app user (shared by the database test binaries). */
#include <mysql.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool admin_name(const char *sql_fmt, const char *name);

/* Create the throwaway database as root and grant it to the app user. */
bool admin_name(const char *sql_fmt, const char *name)
{
    const char *home = getenv("HOME");
    const char *secrets = getenv("SCRIBODO_SECRETS");
    char path[1024], pw[200] = "";
    if (secrets && secrets[0]) snprintf(path, sizeof(path), "%s/db_root_password", secrets);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/db_root_password", home);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    pw[fread(pw, 1, sizeof(pw) - 1, f)] = '\0';
    fclose(f);
    pw[strcspn(pw, "\r\n")] = '\0';
    MYSQL *m = mysql_init(NULL);
    unsigned t = 5;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    const char *configured_port = getenv("SCRIBODO_DB_PORT");
    char *port_end = NULL;
    unsigned long parsed_port = configured_port ? strtoul(configured_port, &port_end, 10) : 33061;
    unsigned port = configured_port && (!configured_port[0] || !port_end || *port_end || parsed_port > 65535) ? 0 : (unsigned)parsed_port;
    bool ok = port && mysql_real_connect(m, "127.0.0.1", "root", pw, NULL, port, NULL, 0) != NULL;
    char sql[512];
    snprintf(sql, sizeof(sql), sql_fmt, name, name);
    ok = ok && mysql_query(m, sql) == 0;
    if (!ok) fprintf(stderr, "admin: %s\n", mysql_error(m));
    mysql_close(m);
    return ok;
}

