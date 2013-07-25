
struct global_state {
    char *app;
    char *unix_path;
    int num_threads;
    int fd;
    int add_dirname_to_path;

    int running;
};

extern struct global_state global_state;
