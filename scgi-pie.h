
#include <pthread.h>
#include <Python.h>

struct thread_data {
    pthread_t thread;

    int dead;
    pthread_cond_t dead_cond;
    pthread_mutex_t dead_mutex;

    PyThreadState *py_thr;
};

struct sp_global_state {
    char *app;
    char *unix_path;
    int unix_mode;
    char *venv;
    int no_venv;
    int buffering;
    int num_threads;
    int fd;
    int add_dirname_to_path;

    struct thread_data *threads;
    int running;
    int reloading;
};

extern struct sp_global_state scgipie_global_state;
#define global_state scgipie_global_state

void pie_init(void);
void pie_main(struct thread_data *data);
void pie_signal_stop(struct thread_data *data);
void pie_finish(void);
