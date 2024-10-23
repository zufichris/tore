#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#ifdef __linux__
#include <unistd.h>
#endif // __linux__

#define BUILD_FOLDER "./build/"

bool build_sqlite3(Nob_Cmd *cmd)
{
    const char *output_path = BUILD_FOLDER"sqlite3.o";
    const char *input_path = "sqlite-amalgamation-3460100/sqlite3.c";
    int rebuild_is_needed = nob_needs_rebuild1(output_path, input_path);
    if (rebuild_is_needed < 0) return false;
    if (rebuild_is_needed) {
        // NOTE: We are omitting extension loading because it depends on dlopen which prevents us from makeing tore statically linked
        nob_cmd_append(cmd, "cc", "-DSQLITE_OMIT_LOAD_EXTENSION", "-O3", "-o", output_path, "-c", input_path);
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    } else {
        nob_log(NOB_INFO, "%s is up to date", output_path);
    }
    return true;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *program_name = shift(argv, argc);
    Nob_Cmd cmd = {0};

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
    if (!build_sqlite3(&cmd)) return 1;
    nob_cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-Wswitch-enum", "-ggdb", "-static", "-I./sqlite-amalgamation-3460100/", "-o", BUILD_FOLDER"tore", "tore.c", BUILD_FOLDER"sqlite3.o");
    if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
    // TODO: bake git hash into executable

    if (argc <= 0) return 0;
    const char *command_name = shift(argv, argc);

    if (strcmp(command_name, "chroot") == 0) {
        // NOTE: this command runs the developed tore with some special
        // environment variables set so it does not damage your "production"
        // database file.
        // NOTE: the name of the command is `chroot` because of historical
        // reasons. It was originally using chroot, but it turned out that just
        // setting a bunch of environment variables is enough. Maybe it should
        // be renamed to something else in the future.
        const char *current_dir = get_current_dir_temp();
        if (current_dir == NULL) return 1;
        if (setenv("HOME", temp_sprintf("%s/"BUILD_FOLDER, current_dir), 1) < 0) {
            nob_log(ERROR, "Could not set variable HOME: %s", strerror(errno));
            return 1;
        }
        if (setenv("TORE_TRACE_MIGRATION_QUERIES", "1", 1) < 0) {
            nob_log(ERROR, "Could not set variable TORE_TRACE_MIGRATION_QUERIES: %s", strerror(errno));
            return 1;
        }
        nob_cmd_append(&cmd, BUILD_FOLDER"tore");
        da_append_many(&cmd, argv, argc);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
        return 0;
    }

    nob_log(ERROR, "Unknown command %s", command_name);
    return 1;
}
