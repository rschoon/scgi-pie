
struct sp_global_state {
    char *app;
    char *unix_path;
    int unix_mode;
    char *venv;
    int no_venv;
    int num_threads;
    int fd;
    int add_dirname_to_path;

    pthread_t *threads;
    int running;
};

extern struct sp_global_state scgipie_global_state;
#define global_state scgipie_global_state
