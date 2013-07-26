
struct sp_global_state {
    char *app;
    char *unix_path;
    char *venv;
    int num_threads;
    int fd;
    int add_dirname_to_path;

    pthread_t *threads;
    int running;
};

extern struct sp_global_state scgipie_global_state;
#define global_state scgipie_global_state
