#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#ifdef __linux__
#include <unistd.h>
#endif // __linux__

#define BUILD_FOLDER "./build/"
#define GIT_HASH_FILE BUILD_FOLDER "git-hash.txt"

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

bool set_environment_variable(const char *name, const char *value)
{
    nob_log(INFO, "SETENV: %s = %s", name, value);
    if (setenv(name, value, 1) < 0) {
        nob_log(ERROR, "Could not set variable %s: %s", name, strerror(errno));
        return false;
    }
    return true;
}

char *get_git_hash(Cmd *cmd)
{
    char *result = NULL;
    String_Builder sb = {0};
    Fd fdout = fd_open_for_write(GIT_HASH_FILE);
    if (fdout == INVALID_FD) return_defer(NULL);
    cmd_append(cmd, "git", "rev-parse", "HEAD");
    if (!cmd_run_sync_redirect_and_reset(cmd, (Nob_Cmd_Redirect) { .fdout = &fdout })) return_defer(NULL);
    if (!read_entire_file(GIT_HASH_FILE, &sb)) return_defer(NULL);
    while (sb.count > 0 && isspace(sb.items[--sb.count]));
    sb_append_null(&sb);
    return_defer(sb.items);
defer:
    if (result == NULL) free(sb.items);
    return result;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *program_name = shift(argv, argc);
    Nob_Cmd cmd = {0};

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
    if (!build_sqlite3(&cmd)) return 1;

    // Templates
    cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-Wswitch-enum", "-ggdb", "-o", BUILD_FOLDER"tt", "tt.c");
    if (!cmd_run_sync_and_reset(&cmd)) return 1;

    Fd index_fd = fd_open_for_write(BUILD_FOLDER"index.h");
    if (index_fd == INVALID_FD) return 1;
    cmd_append(&cmd, BUILD_FOLDER"tt", "./index.h.tt");
    if (!cmd_run_sync_redirect_and_reset(&cmd, (Nob_Cmd_Redirect) {
        .fdout = &index_fd,
    })) return 1;

    char *git_hash = get_git_hash(&cmd);
    cmd_append(&cmd, "cc");
    if (git_hash) {
        cmd_append(&cmd, temp_sprintf("-DGIT_HASH=\"%s\"", git_hash));
        free(git_hash);
    } else {
        cmd_append(&cmd, temp_sprintf("-DGIT_HASH=\"Unknown\""));
    }
    cmd_append(&cmd, "-Wall", "-Wextra", "-Wswitch-enum", "-ggdb", "-static", "-I./sqlite-amalgamation-3460100/", "-I./build/", "-o", BUILD_FOLDER"tore", "tore.c", BUILD_FOLDER"sqlite3.o");

    if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

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
        if (!set_environment_variable("HOME", temp_sprintf("%s/"BUILD_FOLDER, current_dir))) return 1;
        if (!set_environment_variable("TORE_TRACE_MIGRATION_QUERIES", "1")) return 1;
        nob_cmd_append(&cmd, BUILD_FOLDER"tore");
        da_append_many(&cmd, argv, argc);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
        return 0;
    }

    nob_log(ERROR, "Unknown command %s", command_name);
    return 1;
}
