typedef struct {
    const char *name;
    const char *description;
    bool value;
} Flag;

void parse_flags(int *argc, char ***argv, Flag *flags, size_t flags_count)
{
next_flag:
    while (*argc > 0) {
        for (size_t i = 0; i < flags_count; ++i) {
            if (strcmp(flags[i].name, (*argv)[0]) == 0) {
                UNUSED(shift(*argv, *argc));
                flags[i].value = true;
                goto next_flag;
            }
        }
        break;
    }
}

void print_flags(Flag *flags, size_t flags_count)
{
    int max_width = INT_MIN;
    for (size_t i = 0; i < flags_count; ++i) {
        int width = strlen(flags[i].name);
        if (width > max_width) max_width = width;
    }
    for (size_t i = 0; i < flags_count; ++i) {
        printf("  %-*s  %s\n", max_width, flags[i].name, flags[i].description);
    }
}
