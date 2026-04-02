/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is full, unless shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    /* If shutting down, drop the item */
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Insert at tail */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* Wake up any waiting consumer */
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is empty, unless shutting down */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* If shutting down and nothing left, signal done */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Remove from head */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    /* Wake up any waiting producer */
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Build log file path for this container */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, item.container_id);

        /* Write chunk to the container's log file */
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0;
            while (written < (ssize_t)item.length) {
                ssize_t n = write(fd, item.data + written,
                                  item.length - written);
                if (n <= 0) break;
                written += n;
            }
            close(fd);
        }
    }

    /* Drain any remaining items after shutdown signal */
    while (1) {
        pthread_mutex_lock(&ctx->log_buffer.mutex);
        if (ctx->log_buffer.count == 0) {
            pthread_mutex_unlock(&ctx->log_buffer.mutex);
            break;
        }
        log_item_t drain = ctx->log_buffer.items[ctx->log_buffer.head];
        ctx->log_buffer.head =
            (ctx->log_buffer.head + 1) % LOG_BUFFER_CAPACITY;
        ctx->log_buffer.count--;
        pthread_mutex_unlock(&ctx->log_buffer.mutex);

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, drain.container_id);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t w = write(fd, drain.data, drain.length);
            (void)w;
            close(fd);
        }
    }

    return NULL;
}

/*
 * child_fn - container entry point, runs inside cloned namespaces.
 *
 * By the time this runs, clone() has already given it:
 *   - a new PID namespace   (CLONE_NEWPID)
 *   - a new UTS namespace   (CLONE_NEWUTS)
 *   - a new mount namespace (CLONE_NEWNS)
 *
 * Steps:
 *   1. Set hostname to container ID (UTS namespace)
 *   2. Bind-mount rootfs onto itself  (required before chroot)
 *   3. Mount /proc so 'ps' works inside the container
 *   4. chroot into the rootfs
 *   5. Redirect stdout/stderr into the log pipe
 *   6. Apply nice value (used in Task 5 scheduling experiments)
 *   7. execvp the command
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char proc_path[PATH_MAX];

    /* ── 1. Set hostname (UTS namespace) ── */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
        return 1;
    }

    /* ── 2. Bind-mount rootfs onto itself ──
     * chroot requires the target to already be a mountpoint.
     * MS_BIND | MS_REC makes it one without moving anything. */
    if (mount(cfg->rootfs, cfg->rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("mount --bind rootfs");
        return 1;
    }

    /* ── 3. Mount /proc inside the container ──
     * Without procfs, tools like 'ps' or '/proc/self/status' won't work.
     * We mount at <rootfs>/proc before chroot-ing. */
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        perror("mount proc");
        /* non-fatal: container still works, just without /proc */
    }

    /* ── 4. chroot into the rootfs ──
     * The container can no longer see the host filesystem. */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* ── 5. Redirect stdout + stderr into the log pipe ──
     * The supervisor holds the read end. dup2 replaces fd 1 and 2
     * so all container output flows through the bounded-buffer
     * logging pipeline (Task 3). */
    if (cfg->log_write_fd >= 0) {
        if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) perror("dup2 stdout");
        if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) perror("dup2 stderr");
        close(cfg->log_write_fd);
    }

    /* ── 6. Apply nice value (Task 5 scheduling experiments) ── */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");
    }

    /* ── 7. Exec the command ──
     * Split the command string on spaces to build argv. */
    char cmd_copy[CHILD_COMMAND_LEN];
    strncpy(cmd_copy, cfg->command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *exec_argv[64];
    int   exec_argc = 0;
    char *tok = strtok(cmd_copy, " ");
    while (tok && exec_argc < 63) {
        exec_argv[exec_argc++] = tok;
        tok = strtok(NULL, " ");
    }
    exec_argv[exec_argc] = NULL;

    if (exec_argc == 0) {
        fprintf(stderr, "child_fn: empty command\n");
        return 1;
    }

    execvp(exec_argv[0], exec_argv);
    perror("execvp");   /* only reached on failure */
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ── producer thread arg (Task 3) ───────────────────────────── */
typedef struct {
    int pipe_rd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} prod_arg_t;

static void *pipe_producer_thread(void *arg)
{
    prod_arg_t *p = (prod_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while ((n = read(p->pipe_rd, item.data, sizeof(item.data) - 1)) > 0) {
        item.length = (size_t)n;
        item.data[n] = '\0';
        strncpy(item.container_id, p->container_id,
                sizeof(item.container_id) - 1);
        bounded_buffer_push(p->buffer, &item);
    }
    close(p->pipe_rd);
    free(p);
    return NULL;
}

/* ── signal-handler state (global so handler can access it) ─── */
static volatile sig_atomic_t  g_should_stop = 0;
static supervisor_ctx_t      *g_ctx_ptr     = NULL;

static void supervisor_sighandler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_should_stop = 1;
        return;
    }

    if (sig == SIGCHLD) {
        /*
         * Loop with WNOHANG — one SIGCHLD can represent many children.
         * Missing any one causes a zombie. We update the metadata list
         * so the supervisor always knows the true state of each container.
         */
        int   wstatus;
        pid_t pid;
        while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
            if (!g_ctx_ptr) continue;

            pthread_mutex_lock(&g_ctx_ptr->metadata_lock);
            container_record_t *c = g_ctx_ptr->containers;
            while (c) {
                if (c->host_pid == pid) {
                    if (WIFEXITED(wstatus)) {
                        c->exit_code   = WEXITSTATUS(wstatus);
                        c->exit_signal = 0;
                        c->state       = CONTAINER_EXITED;
                    } else if (WIFSIGNALED(wstatus)) {
                        c->exit_code   = -1;
                        c->exit_signal = WTERMSIG(wstatus);
                        /* distinguish graceful stop vs forced kill */
                        c->state = (WTERMSIG(wstatus) == SIGTERM)
                                   ? CONTAINER_STOPPED
                                   : CONTAINER_KILLED;
                    }
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&g_ctx_ptr->metadata_lock);
        }
    }
}

/* ── helper: spawn one container via clone() ─────────────────── */
static int supervisor_launch_container(supervisor_ctx_t *ctx,
                                        const control_request_t *req,
                                        int foreground)
{
    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Allocate metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) { perror("calloc container_record"); return -1; }

    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Create log pipe: container writes to [1], supervisor reads from [0] */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        free(rec);
        return -1;
    }

    /* Build child config */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        perror("calloc child_config");
        close(pipefd[0]); close(pipefd[1]);
        free(rec);
        return -1;
    }
    strncpy(cfg->id,      req->container_id, sizeof(cfg->id)      - 1);
    strncpy(cfg->rootfs,  req->rootfs,       sizeof(cfg->rootfs)  - 1);
    strncpy(cfg->command, req->command,      sizeof(cfg->command) - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];   /* container writes stdout/stderr here */

    /* Allocate clone stack (grows downward, so pass stack_top) */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefd[0]); close(pipefd[1]);
        free(cfg); free(rec);
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    /*
     * clone() with three namespace flags:
     *   CLONE_NEWPID  — container gets its own PID namespace (init = PID 1)
     *   CLONE_NEWUTS  — container gets its own hostname
     *   CLONE_NEWNS   — container gets its own mount table
     *   SIGCHLD       — parent receives SIGCHLD when child exits
     */
    pid_t pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    free(stack);   /* clone copies the stack; we can free our mapping */

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]); close(pipefd[1]);
        free(cfg); free(rec);
        return -1;
    }

    /* Close the write end in the supervisor — the child owns it now */
    close(pipefd[1]);

    /* Update metadata and insert into supervisor list */
    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with the kernel memory monitor (Task 4) */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id,
                               pid, req->soft_limit_bytes,
                               req->hard_limit_bytes);

    fprintf(stdout,
            "[supervisor] started container '%s' pid=%d log=%s\n",
            req->container_id, pid, rec->log_path);
    fflush(stdout);

    if (foreground) {
        /* "run" command: wait for the container and drain its log pipe */
        char buf[LOG_CHUNK_SIZE];
        ssize_t n;
        /* Drain log pipe until container exits */
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            int lfd = open(rec->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lfd >= 0) { (void)write(lfd, buf, n); close(lfd); }
        }
        close(pipefd[0]);

        int wstatus;
        waitpid(pid, &wstatus, 0);
        pthread_mutex_lock(&ctx->metadata_lock);
        if (WIFEXITED(wstatus)) {
            rec->exit_code = WEXITSTATUS(wstatus); rec->exit_signal = 0;
            rec->state = CONTAINER_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
            rec->exit_code = -1; rec->exit_signal = WTERMSIG(wstatus);
            rec->state = CONTAINER_KILLED;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    } else {
        /*
         * Task 3: spawn a producer thread per container.
         * It reads from the log pipe and pushes chunks into the
         * bounded buffer. The logging_thread (consumer) writes to disk.
         */
        typedef struct {
            int pipe_rd;
            char container_id[CONTAINER_ID_LEN];
            bounded_buffer_t *buffer;
        } prod_arg_t;

        prod_arg_t *pa = malloc(sizeof(*pa));
        if (pa) {
            pa->pipe_rd = pipefd[0];
            pa->buffer  = &ctx->log_buffer;
            strncpy(pa->container_id, req->container_id,
                    sizeof(pa->container_id) - 1);
            pthread_t prod_tid;
            pthread_create(&prod_tid, NULL, pipe_producer_thread, pa);
            pthread_detach(prod_tid);
        } else {
            close(pipefd[0]);
        }
    }

    free(cfg);
    return 0;
}

/* ── helper: print container table for 'ps' ─────────────────── */
static void supervisor_print_ps(supervisor_ctx_t *ctx, int out_fd)
{
    char line[512];
    int n;

    n = snprintf(line, sizeof(line),
                 "%-16s %-8s %-12s %-10s %-12s %-12s\n",
                 "ID", "PID", "STATE", "EXIT", "SOFT(MB)", "HARD(MB)");
    (void)write(out_fd, line, n);

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        char started[32];
        struct tm *tm_info = localtime(&c->started_at);
        strftime(started, sizeof(started), "%H:%M:%S", tm_info);

        n = snprintf(line, sizeof(line),
                     "%-16s %-8d %-12s %-10d %-12lu %-12lu\n",
                     c->id,
                     c->host_pid,
                     state_to_string(c->state),
                     c->exit_code,
                     c->soft_limit_bytes / (1024 * 1024),
                     c->hard_limit_bytes / (1024 * 1024));
        write(out_fd, line, n);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx_ptr      = &ctx;

    /* ── mutex + bounded buffer init ── */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init");
                   pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    /* ── 1. Open the kernel memory monitor device (Task 4) ── */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/container_monitor not found "
                "(kernel module not loaded — Task 4 features disabled)\n");

    /* ── 2. Create the UNIX domain socket for CLI IPC ── */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); goto cleanup; }
    chmod(CONTROL_PATH, 0666);

    /* ── 3. Install signal handlers ── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD,  &sa, NULL);
    sigaction(SIGINT,   &sa, NULL);
    sigaction(SIGTERM,  &sa, NULL);

    /* ── 4. Start logging thread (Task 3) ── */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "[supervisor] failed to start logging thread\n");
        goto cleanup;
    }

    fprintf(stdout,
            "[supervisor] running. pid=%d rootfs=%s socket=%s\n",
            getpid(), rootfs, CONTROL_PATH);
    fflush(stdout);

    /* ── 5. Event loop ── */
    while (!g_should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (!FD_ISSET(ctx.server_fd, &rfds)) continue;

        /* Accept one CLI connection */
        int cli_fd = accept(ctx.server_fd, NULL, NULL);
        if (cli_fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        /* Read the control request */
        control_request_t  req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        ssize_t nr = read(cli_fd, &req, sizeof(req));
        if (nr != (ssize_t)sizeof(req)) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "bad request size");
            (void)write(cli_fd, &resp, sizeof(resp));
            close(cli_fd);
            continue;
        }

        /* Dispatch */
        switch (req.kind) {
        case CMD_START:
        case CMD_RUN: {
            int fg = (req.kind == CMD_RUN);
            if (supervisor_launch_container(&ctx, &req, fg) == 0) {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "container '%s' started", req.container_id);
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message),
                         "failed to start '%s'", req.container_id);
            }
            break;
        }
        case CMD_PS:
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "ps follows");
            (void)write(cli_fd, &resp, sizeof(resp));
            supervisor_print_ps(&ctx, cli_fd);
            close(cli_fd);
            continue;

        case CMD_STOP: {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            int found = 0;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0 &&
                    c->state == CONTAINER_RUNNING) {
                    kill(c->host_pid, SIGTERM);
                    c->state = CONTAINER_STOPPED;
                    found = 1;
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            resp.status = found ? 0 : -1;
            snprintf(resp.message, sizeof(resp.message),
                     found ? "stopped '%s'" : "'%s' not found",
                     req.container_id);
            break;
        }
        case CMD_LOGS: {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log",
                     LOG_DIR, req.container_id);
            int lfd = open(path, O_RDONLY);
            if (lfd < 0) {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message),
                         "no log for '%s'", req.container_id);
                (void)write(cli_fd, &resp, sizeof(resp));
            } else {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "log follows");
                (void)write(cli_fd, &resp, sizeof(resp));
                char buf[4096]; ssize_t n;
                while ((n = read(lfd, buf, sizeof(buf))) > 0)
                    (void)write(cli_fd, buf, n);
                close(lfd);
            }
            close(cli_fd);
            continue;
        }
        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "unknown command");
        }

        (void)write(cli_fd, &resp, sizeof(resp));
        close(cli_fd);
    }

    /* ── Orderly shutdown ── */
    fprintf(stdout, "[supervisor] shutting down...\n");

    /* SIGTERM all running containers, give 2s, then SIGKILL */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGTERM);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(2);

    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0);

    /* Free metadata list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stdout, "[supervisor] exited cleanly.\n");
    return 0;
}

/*
 * send_control_request - CLI client path.
 *
 * Connects to the supervisor via UNIX domain socket, sends the
 * control_request_t struct, and prints the response.
 * This is the second IPC mechanism (distinct from the log pipe).
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    /* Read the response header */
    control_response_t resp;
    if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
        fprintf(resp.status == 0 ? stdout : stderr,
                "%s\n", resp.message);
        /* If there is trailing data (logs, ps output) print it */
        char buf[4096]; ssize_t n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            fputs(buf, stdout);
        }
    }

    close(fd);
    return resp.status == 0 ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
