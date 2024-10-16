#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

bool create_schema(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS Notifications (\n"
        "    id INTEGER PRIMARY KEY ASC,\n"
        "    title TEXT NOT NULL,\n"
        "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
        "    dismissed_at DATETIME DEFAULT NULL\n"
        ")\n";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    sql =
        "CREATE TABLE IF NOT EXISTS Reminders (\n"
        "    id INTEGER PRIMARY KEY ASC,\n"
        "    title TEXT NOT NULL,\n"
        "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
        "    scheduled_at DATE NOT NULL,\n"
        "    period TEXT DEFAULT NULL,\n"
        "    finished_at DATETIME DEFAULT NULL\n"
        ")\n";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
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

int main(int argc, char **argv)
{
    int result = 0;
    sqlite3 *db = NULL;

    const char *program_name = shift(argv, argc);
    const char *home_path = getenv("HOME");
    if (home_path == NULL) {
        fprintf(stderr, "ERROR: No $HOME environment variable is setup. We need it to find the location of ~/.tore database.a");
        return_defer(1);
    }

    const char *tore_path = temp_sprintf("%s/.tore", home_path);

    int ret = sqlite3_open(tore_path, &db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", tore_path, sqlite3_errstr(ret));
        return_defer(1);
    }

    if (!create_schema(db)) return_defer(1);

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

    if (strcmp(cmd, "remind") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s remind [<title> <date> [period]]\n", program_name);
            fprintf(stderr, "ERROR: expected title\n");
            return_defer(1);
        }
        const char *title = shift(argv, argc);

        if (argc <= 0) {
            fprintf(stderr, "Usage: %s remind [<title> <date> [period]]\n", program_name);
            fprintf(stderr, "ERROR: expected date\n");
            return_defer(1);
        }

        const char *date = shift(argv, argc);

        const char *period = NULL;
        if (argc > 0) {
            period = shift(argv, argc);
        }

        if (!create_new_reminder(db, title, date, period)) return_defer(1);
        return_defer(0);
    }

    fprintf(stderr, "ERROR: unknown command %s\n", cmd);
    return_defer(1);

defer:
    if (db) sqlite3_close(db);
    return result;
}
