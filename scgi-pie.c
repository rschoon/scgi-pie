
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "scgi-pie.h"

struct global_state global_state = {
    .app = NULL,
    .unix_path = NULL,
    .num_threads = 4,
    .fd = -1,
    .running = 1
};

static struct option longopts[] = {
    { "num-threads",    required_argument,      NULL,       't' },
    { "fd",             required_argument,      NULL,       1000 },
    { "unix",           required_argument,      NULL,       's' },
    { "add-dirname-to-path", no_argument,       NULL,       1001 },
    { NULL,             0,              NULL,       0 }
};

static void usage(void) {
    fprintf(stderr, "...\n");
}

void pie_init(void);
void pie_main(void);
void pie_finish(void);
static void* thread_start(void *arg);
static int create_unix_socket(void);

int main(int argc, char **argv) {
    int ch;
    int i;
    pthread_t *threads;

    while ((ch = getopt_long(argc, argv, "s:t:", longopts, NULL)) != -1) {
        switch (ch) {   
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
            default:
                 usage();
        }
    }
    argc -= optind;
    argv += optind;

    global_state.app = argv[0];

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
     * Create threads
     */

    pie_init();

    threads = malloc(sizeof(pthread_t)*global_state.num_threads);
    for(i = 0; i < global_state.num_threads; i++)
        pthread_create(&threads[i], NULL, thread_start, NULL);

    /*
     * Wait for threads
     */

    for(i = 0; i < global_state.num_threads; i++)
        pthread_join(threads[i], NULL);

    /*
     * Misc cleanup
     */

    free(threads);
    free(global_state.unix_path);

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

