
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "scgi-pie.h"

struct sp_global_state global_state;

static struct option longopts[] = {
    { "num-threads",    required_argument,      NULL,       't' },
    { "fd",             required_argument,      NULL,       1000 },
    { "unix",           required_argument,      NULL,       's' },
    { "unix-mode",      required_argument,      NULL,       'M' },
    { "add-dirname-to-path", no_argument,       NULL,       1001 },
    { "venv",           required_argument,      NULL,       1002 },
    { "no-venv",        no_argument,            NULL,       1003 },
    { NULL,             0,              NULL,       0 }
};

static void usage(void) {
    fprintf(stderr, "Usage: <flags> <app.wsgi>\n"
        "\nProcess Options:\n"
        "--num-threads <num>\n"
        "-t <num>               Number of threads to spawn (defaults to 4)\n"
        "--fd <num>             File descriptor to inherit listen socket from.\n"
        "                       Use with tool such as spawn-fcgi\n"
        "--unix <path>\n"
        "-s <path>              Bind to Unix domain socket on path\n"
        "-M <mode>              Change Unix doman socket path mode\n"
        "\nPython Options:\n"
        "--add-dirname-to-path  Add path of wsgi app to sys.path\n"
        "--venv <path>          Use path as python virtual environment\n"
        "--no-venv              Don't attempt to guess current virtual environment\n"
    );
}

void pie_init(void);
void pie_main(void);
void pie_finish(void);
static void* thread_start(void *arg);
static int create_unix_socket(void);
static void ignore_signal(int sig);
static void register_signal(int sig);

int main(int argc, char **argv) {
    int ch;
    int i;

    memset(&global_state, 0, sizeof(global_state));
    global_state.num_threads = 4;
    global_state.fd = -1;
    global_state.unix_mode = -1;
    global_state.running = 1;

    while ((ch = getopt_long(argc, argv, "M:s:t:", longopts, NULL)) != -1) {
        switch (ch) {
            case 'M':
                global_state.unix_mode = strtol(optarg, NULL, 8);
                break;
            case 's':
                global_state.unix_path = strdup(optarg);
                break;
            case 't':
                global_state.num_threads = strtol(optarg, NULL, 10);
                break;
            case 1000:
                global_state.fd = strtol(optarg, NULL, 10);
                break;
            case 1001:
                global_state.add_dirname_to_path = 1;
                break;
            case 1002:
                global_state.venv = strdup(optarg);
                break;
            case 1003:
                global_state.no_venv = 1;
                break;
            default:
                usage();
                exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    global_state.app = argv[0];

    if(global_state.no_venv && global_state.venv) {
        fprintf(stderr, "Conflicting --no-venv and --venv flags given.\n");
        exit(1);
    }

    /*
     * Sockets
     */

    if(global_state.unix_path != NULL) {
        if(global_state.fd >= 0) {
            fprintf(stderr, "Both unix socket path and file descriptor given, so using file descriptor.\n");
        } else {
            global_state.fd = create_unix_socket();
        }
    }

    if(global_state.fd < 0) {
        fprintf(stderr, "No listener given.\n");
        exit(1);
    }

    if(global_state.app == NULL || *global_state.app == '\0') {
        fprintf(stderr, "No application given.\n");
        exit(1);
    }

    /*
     * Process configuration
     */

    setvbuf(stderr, NULL, _IONBF, 0);

    ignore_signal(SIGPIPE);
    register_signal(SIGINT);
    register_signal(SIGTERM);

    /*
     * Create threads
     */

    pie_init();

    global_state.threads = malloc(sizeof(pthread_t)*global_state.num_threads);
    for(i = 0; i < global_state.num_threads; i++)
        pthread_create(&global_state.threads[i], NULL, thread_start, NULL);

    /*
     * Wait for threads
     */

    for(i = 0; i < global_state.num_threads; i++)
        pthread_join(global_state.threads[i], NULL);

    /*
     * Misc cleanup
     */

    free(global_state.threads);
    free(global_state.unix_path);
    free(global_state.venv);

    return 0;
}

static void* thread_start(void *arg) {
    pie_main();
    return NULL;
}

static int create_unix_socket(void) {
    int socklen;
    int s;
    struct sockaddr_un *sockinfo = NULL;

    unlink(global_state.unix_path);

    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if(s < 0) {
        perror("socket");
        goto error;
    }

    socklen = strlen(global_state.unix_path) + sizeof(sockinfo->sun_family);
    sockinfo = malloc(socklen);
    if(sockinfo == NULL) {
        perror("malloc");
        goto error;
    }

    sockinfo->sun_family = AF_UNIX;
    strcpy(sockinfo->sun_path, global_state.unix_path);
    if(bind(s, (struct sockaddr *)sockinfo, socklen) < 0) {
        perror("bind");
        goto error;
    }

    if(global_state.unix_mode >= 0) {
        if(chmod(global_state.unix_path, global_state.unix_mode) < 0) {
            perror("chmod");
            goto error;
        }
    }

    if(listen(s, SOMAXCONN) < 0) {
        perror("listen");
        goto error;
    }

    goto finish;

error:
    s = -1;
finish:
    free(sockinfo);

    return s;
}

/*
 * Signals
 */

static void signal_handler(int signum) {
    int h;

    switch(signum) {
        case SIGINT:
        case SIGTERM:
            /* any thread might have recieved this */

            if(global_state.running) {
                global_state.running = 0;

                /* force any in progress accept() calls to interrupt */
                for(h = 0; h < global_state.num_threads; h++)
                    pthread_kill(global_state.threads[h], SIGINT);
            }

            break;
    }
}

static void ignore_signal(int signum) {
    struct sigaction siga;

    siga.sa_handler = SIG_IGN;
    sigemptyset(&siga.sa_mask);
    siga.sa_flags = 0;

    sigaction(signum, &siga, NULL);
}

static void register_signal(int signum) {
    struct sigaction siga;

    siga.sa_handler = signal_handler;
    sigemptyset(&siga.sa_mask);
    siga.sa_flags = 0;

    sigaction(signum, &siga, NULL);
}
