#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TICKETS 32
#define QUEUE_CAPACITY 4
#define NAME_SIZE 48
#define ISSUE_SIZE 160
#define STATUS_SIZE 20
#define MESSAGE_SIZE 256
#define LOG_FILE "ticket_log.txt"

typedef struct {
    int id;
    int priority;
    char customer[NAME_SIZE];
    char issue[ISSUE_SIZE];
    char status[STATUS_SIZE];
} Ticket;

typedef struct {
    Ticket tickets[MAX_TICKETS];
    int count;
    int next_id;
    int ids[QUEUE_CAPACITY];
    int front;
    int rear;
    int queued;
} SupportDesk;

typedef struct { char text[MESSAGE_SIZE]; } Message;
static volatile sig_atomic_t interrupted = 0;

static void on_sigint(int signo) { (void)signo; interrupted = 1; }

static void open_log_file_preserved(const char *path, int *fd_out)
{
    struct stat info;
    int existed = stat(path, &info) == 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        *fd_out = -1;
        return;
    }
    if (existed) {
        if (fchmod(fd, info.st_mode & 0777) != 0) perror("preserve ticket log permissions");
    } else if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        perror("set new ticket log permissions");
    }
    *fd_out = fd;
}

static int read_full(int fd, void *buffer, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, (char *)buffer + done, length - done);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    return 1;
}

static int write_full(int fd, const void *buffer, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, (const char *)buffer + done, length - done);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int enqueue(SupportDesk *desk, int id) {
    if (desk->queued == QUEUE_CAPACITY) return 0;
    desk->ids[desk->rear] = id;
    desk->rear = (desk->rear + 1) % QUEUE_CAPACITY;
    desk->queued++;
    return 1;
}

static int dequeue(SupportDesk *desk, int *id) {
    if (desk->queued == 0) return 0;
    *id = desk->ids[desk->front];
    desk->front = (desk->front + 1) % QUEUE_CAPACITY;
    desk->queued--;
    return 1;
}

static void log_event(const Ticket *ticket, const char *event) {
    int fd = -1;
    open_log_file_preserved(LOG_FILE, &fd);
    if (fd < 0) { perror("open ticket log"); return; }
    FILE *file = fdopen(fd, "a");
    if (!file) { perror("fdopen ticket log"); close(fd); return; }
    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32] = "unknown-time";
    if (localtime_r(&now, &tm_now)) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(file, "[%s] #%d customer=\"%s\" priority=%d status=%s event=%s\n",
            stamp, ticket->id, ticket->customer, ticket->priority, ticket->status, event);
    fclose(file);
}

static void ticket_list(const SupportDesk *desk, char *out, size_t cap) {
    size_t used = (size_t)snprintf(out, cap, "Tickets (%d):\n", desk->count);
    for (int i = 0; i < desk->count && used < cap; i++) {
        int n = snprintf(out + used, cap - used, "#%d | %s | priority %d | %s | %s\n",
                         desk->tickets[i].id, desk->tickets[i].customer,
                         desk->tickets[i].priority, desk->tickets[i].status,
                         desk->tickets[i].issue);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
    if (desk->count == 0 && used < cap) snprintf(out + used, cap - used, "No tickets yet.\n");
}

static void handle_command(SupportDesk *desk, const char *cmd, char *out, size_t cap) {
    snprintf(out, cap, "Unknown command. Type /help.");
    if (!strcmp(cmd, "/help")) {
        snprintf(out, cap, "Commands: /new CUSTOMER|ISSUE|PRIORITY, /process, /queue, /list, /stats, /help, /quit. Priority 1=normal, 2=urgent, 3=critical.");
    } else if (!strcmp(cmd, "/list")) {
        ticket_list(desk, out, cap);
    } else if (!strcmp(cmd, "/stats")) {
        int resolved = 0, escalated = 0, waiting = 0;
        for (int i = 0; i < desk->count; i++) {
            resolved += !strcmp(desk->tickets[i].status, "Resolved");
            escalated += !strcmp(desk->tickets[i].status, "Escalated");
            waiting += !strcmp(desk->tickets[i].status, "Waiting");
        }
        snprintf(out, cap, "Statistics: total=%d queued=%d/%d resolved=%d escalated=%d waiting=%d",
                 desk->count, desk->queued, QUEUE_CAPACITY, resolved, escalated, waiting);
    } else if (!strncmp(cmd, "/new ", 5)) {
        char customer[NAME_SIZE] = {0}, issue[ISSUE_SIZE] = {0}, extra;
        int priority = 0;
        if (sscanf(cmd + 5, "%47[^|]|%159[^|]|%d %c", customer, issue, &priority, &extra) != 3 ||
            customer[0] == '\0' || issue[0] == '\0' || priority < 1 || priority > 3) {
            snprintf(out, cap, "Usage: /new CUSTOMER|ISSUE|PRIORITY (priority 1-3).");
        } else if (desk->count == MAX_TICKETS) {
            snprintf(out, cap, "Ticket array is full (%d tickets).", MAX_TICKETS);
        } else {
            Ticket *t = &desk->tickets[desk->count++];
            memset(t, 0, sizeof(*t));
            t->id = desk->next_id++;
            t->priority = priority;
            snprintf(t->customer, sizeof(t->customer), "%s", customer);
            snprintf(t->issue, sizeof(t->issue), "%s", issue);
            snprintf(t->status, sizeof(t->status), "Queued");
            if (enqueue(desk, t->id)) {
                log_event(t, "ticket created and queued");
                snprintf(out, cap, "Created and queued ticket #%d for %s.", t->id, t->customer);
            } else {
                snprintf(t->status, sizeof(t->status), "Waiting");
                log_event(t, "ticket created; small queue full, waiting in array");
                snprintf(out, cap, "Created ticket #%d; queue is full, ticket status is Waiting.", t->id);
            }
        }
    } else if (!strcmp(cmd, "/queue")) {
        int moved = 0;
        for (int i = 0; i < desk->count; i++) {
            Ticket *t = &desk->tickets[i];
            if (!strcmp(t->status, "Waiting")) {
                if (!enqueue(desk, t->id)) break;
                snprintf(t->status, sizeof(t->status), "Queued");
                log_event(t, "waiting ticket added to queue");
                moved++;
            }
        }
        snprintf(out, cap, "Moved %d waiting ticket(s) to the queue (%d/%d occupied).", moved, desk->queued, QUEUE_CAPACITY);
    } else if (!strcmp(cmd, "/process")) {
        int id;
        if (!dequeue(desk, &id)) {
            snprintf(out, cap, "Queue is empty.");
        } else {
            for (int i = 0; i < desk->count; i++) {
                Ticket *t = &desk->tickets[i];
                if (t->id == id) {
                    snprintf(t->status, sizeof(t->status), "%s", t->priority == 3 ? "Escalated" : "Resolved");
                    log_event(t, "ticket processed by support child");
                    snprintf(out, cap, "Processed ticket #%d for %s; status: %s.", t->id, t->customer, t->status);
                    return;
                }
            }
            snprintf(out, cap, "Queued ticket #%d was not found.", id);
        }
    } else if (!strcmp(cmd, "/quit")) {
        snprintf(out, cap, "Support service shutting down.");
    }
}

static void child_service(int request_fd, int response_fd) {
    SupportDesk desk;
    memset(&desk, 0, sizeof(desk));
    desk.next_id = 1;
    for (;;) {
        Message request = {{0}}, reply = {{0}};
        if (read_full(request_fd, &request, sizeof(request)) != 1) break;
        request.text[MESSAGE_SIZE - 1] = '\0';
        handle_command(&desk, request.text, reply.text, sizeof(reply.text));
        if (write_full(response_fd, &reply, sizeof(reply)) < 0) break;
        if (!strcmp(request.text, "/quit")) break;
    }
    close(request_fd);
    close(response_fd);
    _exit(EXIT_SUCCESS);
}

int main(void) {
    int to_child[2], to_parent[2];
    if (pipe(to_child) < 0 || pipe(to_parent) < 0) { perror("pipe"); return EXIT_FAILURE; }
    int log_fd = -1;
    open_log_file_preserved(LOG_FILE, &log_fd);
    if (log_fd < 0) { perror("open ticket log"); return EXIT_FAILURE; }
    close(log_fd);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);

    pid_t child = fork();
    if (child < 0) { perror("fork"); return EXIT_FAILURE; }
    if (child == 0) {
        close(to_child[1]); close(to_parent[0]);
        child_service(to_child[0], to_parent[1]);
    }
    close(to_child[0]); close(to_parent[1]);
    printf("Customer Support Ticket System (array storage; FIFO queue capacity %d; no linked lists)\n", QUEUE_CAPACITY);
    printf("Type /help for commands. Ctrl+C or /quit exits.\n");

    char line[MESSAGE_SIZE];
    while (!interrupted) {
        printf("reception> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (errno == EINTR) { clearerr(stdin); continue; }
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        Message request = {{0}}, reply = {{0}};
        snprintf(request.text, sizeof(request.text), "%s", line);
        if (write_full(to_child[1], &request, sizeof(request)) < 0 || read_full(to_parent[0], &reply, sizeof(reply)) != 1) {
            perror("pipe communication"); break;
        }
        printf("support> %s\n", reply.text);
        if (!strcmp(line, "/quit")) break;
    }
    if (interrupted) {
        Message request = {{0}}, reply = {{0}};
        snprintf(request.text, sizeof(request.text), "/quit");
        if (write_full(to_child[1], &request, sizeof(request)) == 0) (void)read_full(to_parent[0], &reply, sizeof(reply));
        printf("\nInterrupted; shutting down.\n");
    }
    close(to_child[1]); close(to_parent[0]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    printf("Timestamped audit log: %s (existing permissions preserved; new logs are owner-only)\n", LOG_FILE);
    return EXIT_SUCCESS;
}

