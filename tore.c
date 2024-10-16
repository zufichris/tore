#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#define TORE_FILENAME ".tore"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

const char *migrations[] = {
    "CREATE TABLE IF NOT EXISTS Notifications (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    dismissed_at DATETIME DEFAULT NULL\n"
    ");\n",
    "CREATE TABLE IF NOT EXISTS Reminders (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    scheduled_at DATE NOT NULL,\n"
    "    period TEXT DEFAULT NULL,\n"
    "    finished_at DATETIME DEFAULT NULL\n"
    ");\n",
};

// TODO: can we just extract tore_path from db somehow?
bool create_schema(sqlite3 *db, const char *tore_path)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS Migrations (\n"
        "    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
        "    query TEXT NOT NULL\n"
        ");\n";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_prepare_v2(db, "SELECT query FROM Migrations;", -1, &stmt, NULL)!= SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    size_t index = 0;
    int ret = sqlite3_step(stmt);
    for (; ret == SQLITE_ROW; ++index) {
        if (index >= ARRAY_LEN(migrations)) {
            fprintf(stderr, "ERROR: %s: Database scheme is too new. Contains more migrations applied than expected. Update your application.\n", tore_path);
            return_defer(false);
        }
        const char *query = (const char *)sqlite3_column_text(stmt, 0);
        if (strcmp(query, migrations[index]) != 0) {
            fprintf(stderr, "ERROR: %s: Invalid database scheme. Mismatch in migration %zu:\n", tore_path, index);
            fprintf(stderr, "EXPECTED: %s\n", migrations[index]);
            fprintf(stderr, "FOUND: %s\n", query);
            return_defer(false);
        }
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    for (; index < ARRAY_LEN(migrations); ++index) {
        printf("INFO: %s: applying migration %zu\n", tore_path, index);
        printf("%s\n", migrations[index]);
        if (sqlite3_exec(db, migrations[index], NULL, NULL, NULL) != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        int ret = sqlite3_prepare_v2(db, "INSERT INTO Migrations (query) VALUES (?)", -1, &stmt, NULL);
        if (ret != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        if (sqlite3_bind_text(stmt, 1, migrations[index], strlen(migrations[index]), NULL) != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        sqlite3_finalize(stmt);
        stmt = NULL;
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

typedef struct {
    int id;
    const char *title;
    const char *created_at;
} Notification;

typedef struct {
    Notification *items;
    size_t count;
    size_t capacity;
} Notifications;

bool load_active_notifications(sqlite3 *db, Notifications *notifs)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "SELECT id, title, created_at FROM Notifications WHERE dismissed_at IS NULL", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    ret = sqlite3_step(stmt);
    for (int index = 0; ret == SQLITE_ROW; ++index) {
        int id = sqlite3_column_int(stmt, 0);
        // TODO: maybe put all of these things into their own arena
        const char *title = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *created_at = strdup((const char *)sqlite3_column_text(stmt, 2));
        da_append(notifs, ((Notification) {
            .id = id,
            .title = title,
            .created_at = created_at,
        }));
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool show_active_notifications(sqlite3 *db)
{
    bool result = true;

    Notifications notifs = {0};
    if (!load_active_notifications(db, &notifs)) return_defer(false);

    for (size_t i = 0; i < notifs.count; ++i) {
        printf("%zu: %s (%s)\n", i, notifs.items[i].title, notifs.items[i].created_at);
    }

defer:
    free(notifs.items);
    return result;
}

bool dismiss_notification_by_id(sqlite3 *db, int id)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "UPDATE Notifications SET dismissed_at = CURRENT_TIMESTAMP WHERE id = ?", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_bind_int(stmt, 1, id) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool dismiss_notification_by_index(sqlite3 *db, int index)
{
    bool result = true;

    Notifications notifs = {0};
    if (!load_active_notifications(db, &notifs)) return_defer(false);
    if (!(0 <= index && (size_t)index < notifs.count)) {
        fprintf(stderr, "ERROR: %d is not a valid index of an active notification\n", index);
        return_defer(false);
    }
    if (!dismiss_notification_by_id(db, notifs.items[index].id)) return_defer(false);

defer:
    free(notifs.items);
    return result;
}

bool create_notification_with_title(sqlite3 *db, const char *title)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, "INSERT INTO Notifications (title) VALUES (?)", -1, &stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 1, title, strlen(title), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool create_new_reminder(sqlite3 *db, const char *title, const char *scheduled_at, const char *period)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, "INSERT INTO Reminders (title, scheduled_at, period) VALUES (?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 1, title, strlen(title), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 2, scheduled_at, strlen(scheduled_at), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 3, period, period ? strlen(period) : 0, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool fire_off_reminders(sqlite3 *db)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    // Creating new notifications from fired off reminders
    const char *sql = "INSERT INTO Notifications (title) SELECT title FROM Reminders WHERE scheduled_at <= CURRENT_DATE AND finished_at IS NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Finish all the non-periodic reminders
    sql = "UPDATE Reminders SET finished_at = CURRENT_TIMESTAMP WHERE scheduled_at <= CURRENT_DATE AND finished_at IS NULL AND period is NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Reschedule all the period reminders
    sql = "UPDATE Reminders SET scheduled_at = date(scheduled_at, period) WHERE scheduled_at <= CURRENT_DATE AND finished_at IS NULL AND period is NOT NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    sqlite3_finalize(stmt);
    return result;
}

bool show_active_reminders(sqlite3 *db)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "SELECT title, scheduled_at, period FROM Reminders WHERE finished_at IS NULL", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    ret = sqlite3_step(stmt);
    for (int index = 0; ret == SQLITE_ROW; ++index) {
        const char *title = (const char *)sqlite3_column_text(stmt, 0);
        const char *scheduled_at = (const char *)sqlite3_column_text(stmt, 1);
        const char *period = (const char *)sqlite3_column_text(stmt, 2);
        if (period) {
            printf("%s (Scheduled at %s every %s)\n", title, scheduled_at, period);
        } else {
            printf("%s (Scheduled at %s)\n", title, scheduled_at);
        }
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
defer:
    return result;
}

bool verify_date_format(const char *date)
{
    // Who needs Regular Expressions?
    const char *format = "dddd-dd-dd";
    for (; *format && *date; format++, date++) {
        switch (*format) {
            case 'd': if (!isdigit(*date)) return false; break;
            case '-': if (*date != '-')    return false; break;
            default:  UNREACHABLE("verify_date_format");
        }
    }
    return !(*format || *date);
}

int main(int argc, char **argv)
{
    int result = 0;
    sqlite3 *db = NULL;

    const char *program_name = shift(argv, argc);
    const char *home_path = getenv("HOME");
    if (home_path == NULL) {
        fprintf(stderr, "ERROR: No $HOME environment variable is setup. We need it to find the location of ~/"TORE_FILENAME" database.\n");
        return_defer(1);
    }

    const char *tore_path = temp_sprintf("%s/"TORE_FILENAME, home_path);

    int ret = sqlite3_open(tore_path, &db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", tore_path, sqlite3_errstr(ret));
        return_defer(1);
    }

    if (!create_schema(db, tore_path)) return_defer(1);

    if (argc <= 0) {
        if (!fire_off_reminders(db)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        return_defer(0);
    }

    const char *cmd = shift(argv, argc);

    if (strcmp(cmd, "dismiss") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s dismiss <id>\n", program_name);
            fprintf(stderr, "ERROR: expected id\n");
            return_defer(1);
        }

        int index = atoi(shift(argv, argc));
        if (!dismiss_notification_by_index(db, index)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(cmd, "notify") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s notify <title>\n", program_name);
            fprintf(stderr, "ERROR: expected title\n");
            return_defer(1);
        }

        const char *title = shift(argv, argc);

        if (!create_notification_with_title(db, title)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(cmd, "forget") == 0) {
        TODO("remove reminders");
        return_defer(0);
    }

    if (strcmp(cmd, "remind") == 0) {
        if (argc <= 0) {
            if (!show_active_reminders(db)) return_defer(1);
            return_defer(0);
        }

        const char *title = shift(argv, argc);
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s remind [<title> <scheduled_at> [period]]\n", program_name);
            fprintf(stderr, "ERROR: expected scheduled_at\n");
            return_defer(1);
        }

        // TODO: research if it's possible to enforce the date format on the level of sqlite3 contraints
        const char *scheduled_at = shift(argv, argc); 
        if (!verify_date_format(scheduled_at)) {
            fprintf(stderr, "ERROR: %s is not a valid date format\n", scheduled_at);
            return_defer(1);
        }

        // TODO: verify the format of period during parsing of the CLI arguments
        const char *period = NULL;
        if (argc > 0) {
            period = shift(argv, argc);
        }

        if (!create_new_reminder(db, title, scheduled_at, period)) return_defer(1);
        if (!show_active_reminders(db)) return_defer(1);
        return_defer(0);
    }

    fprintf(stderr, "ERROR: unknown command %s\n", cmd);
    return_defer(1);

defer:
    if (db) sqlite3_close(db);
    return result;
}
