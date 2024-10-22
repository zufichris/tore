#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sqlite3.h"

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#define TORE_FILENAME ".tore"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

const char *migrations[] = {
    // Initial scheme
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

    // Add reference to the Reminder that created the Notification
    "ALTER TABLE Notifications RENAME TO Notifications_old;\n"
    "CREATE TABLE IF NOT EXISTS Notifications (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    dismissed_at DATETIME DEFAULT NULL,\n"
    "    reminder_id INTEGER DEFAULT NULL,\n"
    "    FOREIGN KEY (reminder_id) REFERENCES Reminders(id)\n"
    ");\n"
    "INSERT INTO Notifications (id, title, created_at, dismissed_at)\n"
    "SELECT id, title, created_at, dismissed_at FROM Notifications_old;\n"
    "DROP TABLE Notifications_old;\n"
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

    bool tore_trace_migration_queries = getenv("TORE_TRACE_MIGRATION_QUERIES") != NULL;
    for (; index < ARRAY_LEN(migrations); ++index) {
        printf("INFO: %s: applying migration %zu\n", tore_path, index);
        if (tore_trace_migration_queries) printf("%s\n", migrations[index]);
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

    int ret = sqlite3_prepare_v2(db, "SELECT id, title, datetime(created_at, 'localtime') FROM Notifications WHERE dismissed_at IS NULL", -1, &stmt, NULL);
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

typedef struct {
    int id;
    const char *title;
    const char *scheduled_at;
    const char *period;
} Reminder;

typedef struct {
    Reminder *items;
    size_t count;
    size_t capacity;
} Reminders;

// TODO: sort reminders by how close they are to today's date
bool load_active_reminders(sqlite3 *db, Reminders *reminders)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "SELECT id, title, scheduled_at, period FROM Reminders WHERE finished_at IS NULL", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    ret = sqlite3_step(stmt);
    for (int index = 0; ret == SQLITE_ROW; ++index) {
        int id = sqlite3_column_int(stmt, 0);
        const char *title = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *scheduled_at = strdup((const char *)sqlite3_column_text(stmt, 2));
        const char *period = (const char *)sqlite3_column_text(stmt, 3);
        if (period != NULL) period = strdup(period);
        da_append(reminders, ((Reminder) {
            .id = id,
            .title = title,
            .scheduled_at = scheduled_at,
            .period = period,
        }));
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
defer:
    return result;
}

typedef enum {
    PERIOD_NONE = -1,
    PERIOD_DAY,
    PERIOD_WEEK,
    PERIOD_MONTH,
    PERIOD_YEAR,
    COUNT_PERIODS,
} Period;

typedef struct {
    const char *modifier;
    const char *name;
} Period_Modifier;

static_assert(COUNT_PERIODS == 4, "Amount of periods have changed");
Period_Modifier tore_period_modifiers[COUNT_PERIODS] = {
    [PERIOD_DAY]   = { .modifier = "d", .name = "days"   },
    [PERIOD_WEEK]  = { .modifier = "w", .name = "weeks"  },
    [PERIOD_MONTH] = { .modifier = "m", .name = "months" },
    [PERIOD_YEAR]  = { .modifier = "y", .name = "years"  },
};

Period period_by_tore_modifier(const char *modifier)
{
    for (Period period = PERIOD_NONE; period < COUNT_PERIODS; ++period) {
        if (strcmp(modifier, tore_period_modifiers[period].modifier) == 0) {
            return period;
        }
    }
    return PERIOD_NONE;
}

const char *render_period_as_sqlite3_datetime_modifier_temp(Period period, unsigned long period_length)
{
    switch (period) {
    case PERIOD_NONE:  return NULL;
    case PERIOD_DAY:   return temp_sprintf("+%lu days",   period_length);
    case PERIOD_WEEK:  return temp_sprintf("+%lu days",   period_length*7);
    case PERIOD_MONTH: return temp_sprintf("+%lu months", period_length);
    case PERIOD_YEAR:  return temp_sprintf("+%lu years",  period_length);
    case COUNT_PERIODS:
    default: UNREACHABLE("render_period_as_sqlite3_datetime_modifier_temp");
    }
}

bool create_new_reminder(sqlite3 *db, const char *title, const char *scheduled_at, Period period, unsigned long period_length)
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
    const char *rendered_period = render_period_as_sqlite3_datetime_modifier_temp(period, period_length);
    if (sqlite3_bind_text(stmt, 3, rendered_period, rendered_period ? strlen(rendered_period) : 0, NULL) != SQLITE_OK) {
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

// NOTE: The general policy of the application is that all the date times are stored in GMT, but before displaying them and/or making logical decisions upon them they are converted to localtime.
bool fire_off_reminders(sqlite3 *db)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    // Creating new notifications from fired off reminders
    const char *sql = "INSERT INTO Notifications (title, reminder_id) SELECT title, id FROM Reminders WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Finish all the non-periodic reminders
    sql = "UPDATE Reminders SET finished_at = CURRENT_TIMESTAMP WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL AND period is NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Reschedule all the period reminders
    sql = "UPDATE Reminders SET scheduled_at = date(scheduled_at, period) WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL AND period is NOT NULL";
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

    Reminders reminders = {0};

    if (!load_active_reminders(db, &reminders)) return_defer(false);
    for (size_t i = 0; i < reminders.count; ++i) {
        Reminder *it = &reminders.items[i];
        if (it->period) {
            fprintf(stderr, "%zu: %s (Scheduled at %s every %s)\n", i, it->title, it->scheduled_at, it->period);
        } else {
            fprintf(stderr, "%zu: %s (Scheduled at %s)\n", i, it->title, it->scheduled_at);
        }
    }

defer:
    free(reminders.items);
    return result;
}

bool remove_reminder_by_id(sqlite3 *db, int id)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "UPDATE Reminders SET finished_at = CURRENT_TIMESTAMP WHERE id = ?", -1, &stmt, NULL);
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

bool remove_reminder_by_number(sqlite3 *db, int number)
{
    bool result = true;

    Reminders reminders = {0};
    if (!load_active_reminders(db, &reminders)) return_defer(false);
    if (!(0 <= number && (size_t)number < reminders.count)) {
        fprintf(stderr, "ERROR: %d is not a valid index of a reminder\n", number);
        return_defer(false);
    }
    if (!remove_reminder_by_id(db, reminders.items[number].id)) return_defer(false);

defer:
    free(reminders.items);
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

    srand(time(0));

    const char *program_name = shift(argv, argc);

    const char *command_name= "checkout";
    if (argc > 0) command_name = shift(argv, argc);

    // TODO: implement `help` command

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

    // TODO: `undo` command

    if (strcmp(command_name, "checkout") == 0) {
        if (!fire_off_reminders(db)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        // TODO: show reminders that are about to fire off
        //   Maybe they should fire off a "warning" notification before doing the main one?
        return_defer(0);
    }

    if (strcmp(command_name, "dismiss") == 0) {
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

    if (strcmp(command_name, "notify") == 0) {
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

    if (strcmp(command_name, "forget") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s forget <number>\n", program_name);
            fprintf(stderr, "ERROR: expected number\n");
            return_defer(1);
        }
        int number = atoi(shift(argv, argc));
        if (!remove_reminder_by_number(db, number)) return_defer(1);
        if (!show_active_reminders(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(command_name, "remind") == 0) {
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

        // TODO: Allow the scheduled_at to be things like "today", "tomorrow", etc
        // TODO: research if it's possible to enforce the date format on the level of sqlite3 contraints
        const char *scheduled_at = shift(argv, argc);
        if (!verify_date_format(scheduled_at)) {
            fprintf(stderr, "ERROR: %s is not a valid date format\n", scheduled_at);
            return_defer(1);
        }

        Period period = PERIOD_NONE;
        unsigned long period_length = 0;
        if (argc > 0) {
            const char *unparsed_period = shift(argv, argc);
            char *endptr = NULL;
            period_length = strtoul(unparsed_period, &endptr, 10);
            if (endptr == unparsed_period) {
                fprintf(stderr, "ERROR: Invalid period `%s`. Expected something like\n", unparsed_period);
                for (Period p = 0; p < COUNT_PERIODS; ++p) {
                    Period_Modifier *pm = &tore_period_modifiers[p];
                    size_t l = rand()%9 + 1;
                    fprintf(stderr, "    %lu%s - means every %lu %s\n", l, pm->modifier, l, pm->name);
                }
                return_defer(1);
            }
            unparsed_period = endptr;
            period = period_by_tore_modifier(unparsed_period);
            if (period == PERIOD_NONE) {
                fprintf(stderr, "ERROR: Unknown period modifier `%s`. Expected modifiers are\n", unparsed_period);
                for (Period p = 0; p < COUNT_PERIODS; ++p) {
                    Period_Modifier *pm = &tore_period_modifiers[p];
                    fprintf(stderr, "    %lu%s  - means every %lu %s\n", period_length, pm->modifier, period_length, pm->name);
                }
                return_defer(1);
            }
        }

        if (!create_new_reminder(db, title, scheduled_at, period, period_length)) return_defer(1);
        if (!show_active_reminders(db)) return_defer(1);
        return_defer(0);
    }

    fprintf(stderr, "ERROR: unknown command %s\n", command_name);
    return_defer(1);

defer:
    if (db) sqlite3_close(db);
    return result;
}
