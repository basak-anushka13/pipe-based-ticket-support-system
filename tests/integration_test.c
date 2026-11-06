#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTPUT_SIZE 65536

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        (void)fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int contains(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static int run_program(const char *binary, const char *const arguments[], const char *input,
                       char *output, size_t output_capacity, char workdir[64])
{
    (void)snprintf(workdir, 64, "/tmp/ticket-c-test-XXXXXX");
    if (mkdtemp(workdir) == NULL) return -1;
    char input_path[128], output_path[128];
    (void)snprintf(input_path, sizeof(input_path), "%s/input", workdir);
    (void)snprintf(output_path, sizeof(output_path), "%s/output", workdir);
    int in_fd = open(input_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) (void)close(in_fd);
        if (out_fd >= 0) (void)close(out_fd);
        return -1;
    }
    size_t input_length = strlen(input);
    size_t written = 0;
    while (written < input_length) {
        ssize_t count = write(in_fd, input + written, input_length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { (void)close(in_fd); (void)close(out_fd); return -1; }
        written += (size_t)count;
    }
    (void)close(in_fd);

    pid_t child = fork();
    if (child < 0) { (void)close(out_fd); return -1; }
    if (child == 0) {
        if (chdir(workdir) < 0 || dup2(out_fd, STDOUT_FILENO) < 0 ||
            dup2(out_fd, STDERR_FILENO) < 0 || dup2(open(input_path, O_RDONLY), STDIN_FILENO) < 0)
            _exit(126);
        (void)close(out_fd);
        char *exec_arguments[8];
        size_t index = 0;
        exec_arguments[index++] = (char *)binary;
        while (arguments[index - 1] != NULL && index < 7) {
            exec_arguments[index] = (char *)arguments[index - 1];
            ++index;
        }
        exec_arguments[index] = NULL;
        execv(binary, exec_arguments);
        _exit(127);
    }
    (void)close(out_fd);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    int read_fd = open(output_path, O_RDONLY);
    if (read_fd < 0) return -1;
    size_t used = 0;
    while (used + 1 < output_capacity) {
        ssize_t count = read(read_fd, output + used, output_capacity - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
    }
    output[used] = '\0';
    (void)close(read_fd);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int read_file_contains(const char *path, const char *needle)
{
    char content[OUTPUT_SIZE];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t count = read(fd, content, sizeof(content) - 1);
    (void)close(fd);
    if (count < 0) return 0;
    content[count] = '\0';
    return contains(content, needle);
}

static void remove_workdir(const char *workdir)
{
    static const char *const names[] = {"input", "output", "ticket_log.txt", "chat_log.txt"};
    char path[128];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        (void)snprintf(path, sizeof(path), "%s/%s", workdir, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(workdir);
}

static void test_ticket_application(const char *binary)
{
    static const char *const arguments[] = {NULL};
    const char *input = "/help\n/new Alice|Cannot log in|1\n/new Bob|Payment issue|2\n"
                        "/new Cara|Urgent outage|3\n/new Dan|Account locked|1\n"
                        "/new Eve|Queue overflow case|1\n/stats\n/process\n/queue\n"
                        "/process\n/process\n/list\n/stats\n/quit\n";
    char output[OUTPUT_SIZE], workdir[64];
    int result = run_program(binary, arguments, input, output, sizeof(output), workdir);
    check(result == 0, "ticket application exits successfully");
    check(contains(output, "Created and queued ticket #1 for Alice."), "ticket creation works");
    check(contains(output, "Created ticket #5; queue is full"), "bounded queue overflow is handled");
    check(contains(output, "status: Resolved"), "ticket processing works");
    char log_path[128];
    (void)snprintf(log_path, sizeof(log_path), "%s/ticket_log.txt", workdir);
    check(read_file_contains(log_path, "ticket created and queued"), "ticket audit log records creation");
    remove_workdir(workdir);
}

static void test_advanced_chat(const char *binary, const char *const arguments[], const char *input,
                               const char *screen_text, const char *request_text,
                               const char *reply_text, const char *label)
{
    char output[OUTPUT_SIZE], workdir[64], log_path[128];
    int result = run_program(binary, arguments, input, output, sizeof(output), workdir);
    check(result == 0, label);
    check(contains(output, screen_text), "chat demo displays expected response");
    (void)snprintf(log_path, sizeof(log_path), "%s/chat_log.txt", workdir);
    check(read_file_contains(log_path, request_text), "chat log records the sent message");
    check(read_file_contains(log_path, reply_text), "chat log records the received reply");
    remove_workdir(workdir);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s ticket_system ipc_advanced\n", argv[0]);
        return 2;
    }
    test_ticket_application(argv[1]);

    char room[32];
    (void)snprintf(room, sizeof(room), "test-%ld", (long)getpid());
    const char *room_arguments[] = {"rooms", room, NULL};
    test_advanced_chat(argv[2], room_arguments, "test-passphrase\nhello room\n/quit\n",
                       "support: hello room", "parent-to-child: hello room",
                       "child-to-parent: support: hello room", "encrypted FIFO room succeeds");
    const char *server_arguments[] = {"server", NULL};
    test_advanced_chat(argv[2], server_arguments, "/send 2 reset password\n/quit\n",
                       "agent-2: reset password", "parent-to-child: reset password",
                       "child-to-parent: agent-2: reset password", "multi-child server succeeds");
    const char *signal_arguments[] = {"signal-chat", NULL};
    test_advanced_chat(argv[2], signal_arguments, "hello signal\n/quit\n",
                       "signal child received: hello signal", "parent-to-child: hello signal",
                       "child-to-parent: signal child received: hello signal", "signal chat succeeds");

    if (failures != 0) {
        (void)fprintf(stderr, "%d C integration check(s) failed.\n", failures);
        return 1;
    }
    (void)puts("C integration tests passed.");
    return 0;
}
