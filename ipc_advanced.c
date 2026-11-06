#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEXT_SIZE 256
#define CHAT_WORKERS 3
#define KEY_SIZE 32
#define SALT_SIZE 16
#define NONCE_SIZE 12
#define TAG_SIZE 16
#define PBKDF2_ROUNDS 200000
#define ROOM_NAME_SIZE 33
#define CHAT_LOG_FILE "chat_log.txt"

typedef struct { char text[TEXT_SIZE]; } Message;
typedef struct {
    uint32_t length;
    unsigned char nonce[NONCE_SIZE];
    unsigned char tag[TAG_SIZE];
    unsigned char ciphertext[TEXT_SIZE];
} SecureMessage;

static void log_chat_message(const char *channel, const char *direction, const char *text)
{
    int fd = open(CHAT_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        perror("open chat log");
        return;
    }
    FILE *file = fdopen(fd, "a");
    if (file == NULL) {
        perror("fdopen chat log");
        (void)close(fd);
        return;
    }
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32] = "unknown-time";
    if (localtime_r(&now, &local_time) != NULL)
        (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    (void)fprintf(file, "[%s] channel=%s %s: %s\n", timestamp, channel, direction, text);
    if (fclose(file) != 0) perror("write chat log");
}

static int read_full(int fd, void *buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = read(fd, (char *)buffer + done, length - done);
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)count;
    }
    return 1;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, (const char *)buffer + done, length - done);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

static int read_line(char *buffer, size_t capacity)
{
    if (fgets(buffer, (int)capacity, stdin) == NULL) return 0;
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

static int derive_key(const char *passphrase, const unsigned char salt[SALT_SIZE], unsigned char key[KEY_SIZE])
{
    return PKCS5_PBKDF2_HMAC(passphrase, (int)strlen(passphrase), salt, SALT_SIZE,
                             PBKDF2_ROUNDS, EVP_sha256(), KEY_SIZE, key) == 1 ? 0 : -1;
}

static int encrypt_message(const unsigned char key[KEY_SIZE], const char *plaintext, SecureMessage *message)
{
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int length = 0, final_length = 0, result = -1;
    size_t plain_length = strlen(plaintext);
    if (context == NULL || plain_length >= TEXT_SIZE || RAND_bytes(message->nonce, NONCE_SIZE) != 1) goto done;
    if (EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, NULL) != 1 ||
        EVP_EncryptInit_ex(context, NULL, NULL, key, message->nonce) != 1 ||
        EVP_EncryptUpdate(context, message->ciphertext, &length,
                          (const unsigned char *)plaintext, (int)plain_length) != 1 ||
        EVP_EncryptFinal_ex(context, message->ciphertext + length, &final_length) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, message->tag) != 1) goto done;
    message->length = (uint32_t)(length + final_length);
    result = 0;
done:
    EVP_CIPHER_CTX_free(context);
    return result;
}

static int decrypt_message(const unsigned char key[KEY_SIZE], const SecureMessage *message, char plaintext[TEXT_SIZE])
{
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int length = 0, final_length = 0, result = -1;
    if (context == NULL || message->length >= TEXT_SIZE) goto done;
    if (EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, NULL) != 1 ||
        EVP_DecryptInit_ex(context, NULL, NULL, key, message->nonce) != 1 ||
        EVP_DecryptUpdate(context, (unsigned char *)plaintext, &length,
                          message->ciphertext, (int)message->length) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, (void *)message->tag) != 1 ||
        EVP_DecryptFinal_ex(context, (unsigned char *)plaintext + length, &final_length) != 1) goto done;
    plaintext[length + final_length] = '\0';
    result = 0;
done:
    EVP_CIPHER_CTX_free(context);
    return result;
}

static int valid_room_name(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= ROOM_NAME_SIZE) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void encrypted_peer(int read_fd, int write_fd, const unsigned char key[KEY_SIZE])
{
    for (;;) {
        SecureMessage incoming = {0}, outgoing = {0};
        char plaintext[TEXT_SIZE], reply[TEXT_SIZE];
        if (read_full(read_fd, &incoming, sizeof(incoming)) != 1) break;
        if (decrypt_message(key, &incoming, plaintext) < 0) {
            fprintf(stderr, "Peer rejected an unauthenticated/invalid message.\n");
            break;
        }
        (void)snprintf(reply, sizeof(reply), "support: %.240s", plaintext);
        if (encrypt_message(key, reply, &outgoing) < 0 ||
            write_full(write_fd, &outgoing, sizeof(outgoing)) < 0) break;
        if (strcmp(plaintext, "/quit") == 0) break;
    }
    (void)close(read_fd);
    (void)close(write_fd);
    _exit(EXIT_SUCCESS);
}

static int demo_rooms(const char *room)
{
    char incoming_path[128], outgoing_path[128], passphrase[TEXT_SIZE] = {0};
    unsigned char salt[SALT_SIZE], key[KEY_SIZE] = {0};
    int write_fd = -1, read_fd = -1, created_in = 0, created_out = 0, result = 0;
    pid_t child = -1;

    if (!valid_room_name(room)) {
        fprintf(stderr, "Room names must be 1-%d letters, digits, '_' or '-'.\n", ROOM_NAME_SIZE - 1);
        return 2;
    }
    if (snprintf(incoming_path, sizeof(incoming_path), "/tmp/ticket-%s-in.fifo", room) >= (int)sizeof(incoming_path) ||
        snprintf(outgoing_path, sizeof(outgoing_path), "/tmp/ticket-%s-out.fifo", room) >= (int)sizeof(outgoing_path)) return 2;

    fputs("Room passphrase (visible input): ", stdout);
    (void)fflush(stdout);
    if (!read_line(passphrase, sizeof(passphrase)) || passphrase[0] == '\0' ||
        RAND_bytes(salt, sizeof(salt)) != 1 || derive_key(passphrase, salt, key) < 0) {
        fprintf(stderr, "Could not initialize room encryption.\n");
        result = 1;
        goto cleanup;
    }
    if (mkfifo(incoming_path, 0600) < 0) {
        perror("mkfifo room input (room may already exist)");
        result = 1;
        goto cleanup;
    }
    created_in = 1;
    if (mkfifo(outgoing_path, 0600) < 0) {
        perror("mkfifo room output (room may already exist)");
        result = 1;
        goto cleanup;
    }
    created_out = 1;

    child = fork();
    if (child < 0) {
        perror("fork");
        result = 1;
        goto cleanup;
    }
    if (child == 0) {
        int child_read = open(incoming_path, O_RDONLY);
        int child_write = open(outgoing_path, O_WRONLY);
        if (child_read < 0 || child_write < 0) _exit(EXIT_FAILURE);
        encrypted_peer(child_read, child_write, key);
    }

    write_fd = open(incoming_path, O_WRONLY);
    read_fd = open(outgoing_path, O_RDONLY);
    if (write_fd < 0 || read_fd < 0) {
        perror("open room FIFO");
        result = 1;
        goto cleanup;
    }
    printf("Encrypted FIFO room '%s'. Type /quit to leave. Chat log: %s\n", room, CHAT_LOG_FILE);
    for (;;) {
        char plaintext[TEXT_SIZE], reply[TEXT_SIZE];
        SecureMessage request = {0}, response = {0};
        fputs("you> ", stdout);
        (void)fflush(stdout);
        if (!read_line(plaintext, sizeof(plaintext))) break;
        if (encrypt_message(key, plaintext, &request) < 0 ||
            write_full(write_fd, &request, sizeof(request)) < 0) {
            fprintf(stderr, "Encrypted room request failed.\n");
            result = 1;
            break;
        }
        log_chat_message("fifo-room", "parent-to-child", plaintext);
        if (read_full(read_fd, &response, sizeof(response)) != 1 ||
            decrypt_message(key, &response, reply) < 0) {
            fprintf(stderr, "Encrypted room response failed (invalid tag or FIFO I/O error).\n");
            result = 1;
            break;
        }
        log_chat_message("fifo-room", "child-to-parent", reply);
        puts(reply);
        if (strcmp(plaintext, "/quit") == 0) break;
    }

cleanup:
    if (write_fd >= 0) (void)close(write_fd);
    if (read_fd >= 0) (void)close(read_fd);
    if (child > 0) while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
    if (created_in) (void)unlink(incoming_path);
    if (created_out) (void)unlink(outgoing_path);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(passphrase, sizeof(passphrase));
    return result;
}

static void worker_loop(int request_fd, int response_fd, unsigned int worker_id)
{
    for (;;) {
        Message request = {{0}}, reply = {{0}};
        if (read_full(request_fd, &request, sizeof(request)) != 1) break;
        request.text[TEXT_SIZE - 1] = '\0';
        (void)snprintf(reply.text, sizeof(reply.text), "agent-%u: %.240s", worker_id, request.text);
        if (write_full(response_fd, &reply, sizeof(reply)) < 0 || strcmp(request.text, "/quit") == 0) break;
    }
    (void)close(request_fd);
    (void)close(response_fd);
    _exit(EXIT_SUCCESS);
}

static int demo_server(void)
{
    int requests[CHAT_WORKERS][2], responses[CHAT_WORKERS][2];
    pid_t children[CHAT_WORKERS];
    for (int i = 0; i < CHAT_WORKERS; ++i) {
        if (pipe(requests[i]) < 0 || pipe(responses[i]) < 0) { perror("worker pipe"); return 1; }
    }
    for (int i = 0; i < CHAT_WORKERS; ++i) {
        children[i] = fork();
        if (children[i] < 0) { perror("fork worker"); return 1; }
        if (children[i] == 0) {
            for (int j = 0; j < CHAT_WORKERS; ++j) {
                (void)close(requests[j][1]);
                (void)close(responses[j][0]);
                if (j != i) { (void)close(requests[j][0]); (void)close(responses[j][1]); }
            }
            worker_loop(requests[i][0], responses[i][1], (unsigned int)i + 1U);
        }
    }
    for (int i = 0; i < CHAT_WORKERS; ++i) { (void)close(requests[i][0]); (void)close(responses[i][1]); }
    puts("Multi-child support chat. Use /send 1..3 MESSAGE or /quit. Chat log: " CHAT_LOG_FILE);
    int result = 0;
    char line[TEXT_SIZE];
    for (;;) {
        fputs("server> ", stdout); (void)fflush(stdout);
        if (!read_line(line, sizeof(line)) || strcmp(line, "/quit") == 0) break;
        int worker = 0;
        char text[TEXT_SIZE] = {0};
        if (sscanf(line, "/send %d %255[^\n]", &worker, text) != 2 || worker < 1 || worker > CHAT_WORKERS) {
            fprintf(stderr, "Usage: /send WORKER_ID MESSAGE (worker IDs 1-%d)\n", CHAT_WORKERS);
            continue;
        }
        Message request = {{0}}, reply = {{0}};
        (void)snprintf(request.text, sizeof(request.text), "%s", text);
        if (write_full(requests[worker - 1][1], &request, sizeof(request)) < 0) {
            perror("worker request"); result = 1; break;
        }
        log_chat_message("worker-server", "parent-to-child", request.text);
        if (read_full(responses[worker - 1][0], &reply, sizeof(reply)) != 1) {
            perror("worker response"); result = 1; break;
        }
        log_chat_message("worker-server", "child-to-parent", reply.text);
        puts(reply.text);
    }
    for (int i = 0; i < CHAT_WORKERS; ++i) {
        Message stop = {{0}}, reply = {{0}};
        (void)snprintf(stop.text, sizeof(stop.text), "/quit");
        if (write_full(requests[i][1], &stop, sizeof(stop)) == 0) (void)read_full(responses[i][0], &reply, sizeof(reply));
        (void)close(requests[i][1]); (void)close(responses[i][0]);
    }
    for (int i = 0; i < CHAT_WORKERS; ++i) while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) { }
    return result;
}

static int demo_signal_chat(void)
{
    sigset_t signal_set;
    int request_pipe[2], response_pipe[2];
    if (sigemptyset(&signal_set) < 0 || sigaddset(&signal_set, SIGUSR1) < 0 ||
        sigprocmask(SIG_BLOCK, &signal_set, NULL) < 0 || pipe(request_pipe) < 0 || pipe(response_pipe) < 0) {
        perror("signal chat setup"); return 1;
    }
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        (void)close(request_pipe[1]); (void)close(response_pipe[0]);
        for (;;) {
            Message request = {{0}}, reply = {{0}};
            if (read_full(request_pipe[0], &request, sizeof(request)) != 1) break;
            request.text[TEXT_SIZE - 1] = '\0';
            (void)snprintf(reply.text, sizeof(reply.text), "signal child received: %.220s", request.text);
            if (write_full(response_pipe[1], &reply, sizeof(reply)) < 0) break;
            (void)kill(getppid(), SIGUSR1);
            if (strcmp(request.text, "/quit") == 0) break;
        }
        _exit(EXIT_SUCCESS);
    }
    (void)close(request_pipe[0]); (void)close(response_pipe[1]);
    puts("Signal-controlled pipe chat. Type /quit to leave. Chat log: " CHAT_LOG_FILE);
    int result = 0;
    char line[TEXT_SIZE];
    for (;;) {
        fputs("you> ", stdout); (void)fflush(stdout);
        if (!read_line(line, sizeof(line))) break;
        Message request = {{0}}, reply = {{0}};
        (void)snprintf(request.text, sizeof(request.text), "%s", line);
        if (write_full(request_pipe[1], &request, sizeof(request)) < 0) { result = 1; break; }
        log_chat_message("signal-chat", "parent-to-child", request.text);
        int received = 0;
        if (sigwait(&signal_set, &received) != 0 || received != SIGUSR1 ||
            read_full(response_pipe[0], &reply, sizeof(reply)) != 1) {
            fprintf(stderr, "signal chat response failed\n"); result = 1; break;
        }
        log_chat_message("signal-chat", "child-to-parent", reply.text);
        puts(reply.text);
        if (strcmp(line, "/quit") == 0) break;
    }
    (void)close(request_pipe[1]); (void)close(response_pipe[0]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
    return result;
}

static void print_hex(const char *label, const unsigned char *data, size_t length)
{
    printf("%s", label);
    for (size_t i = 0; i < length; ++i) printf("%02x", (unsigned int)data[i]);
    putchar('\n');
}

static int demo_encrypt(void)
{
    char passphrase[TEXT_SIZE] = {0}, plaintext[TEXT_SIZE] = {0}, recovered[TEXT_SIZE];
    unsigned char salt[SALT_SIZE], key[KEY_SIZE] = {0};
    SecureMessage message = {0};
    int result = 1;
    fputs("Passphrase (visible input): ", stdout); (void)fflush(stdout);
    if (!read_line(passphrase, sizeof(passphrase)) || passphrase[0] == '\0' ||
        RAND_bytes(salt, sizeof(salt)) != 1 || derive_key(passphrase, salt, key) < 0) {
        fprintf(stderr, "Key setup failed.\n"); goto done;
    }
    fputs("Message: ", stdout); (void)fflush(stdout);
    if (!read_line(plaintext, sizeof(plaintext)) || encrypt_message(key, plaintext, &message) < 0 ||
        decrypt_message(key, &message, recovered) < 0) {
        fprintf(stderr, "Encryption/decryption failed.\n"); goto done;
    }
    print_hex("salt: ", salt, sizeof(salt));
    print_hex("nonce: ", message.nonce, sizeof(message.nonce));
    print_hex("ciphertext: ", message.ciphertext, message.length);
    print_hex("tag: ", message.tag, sizeof(message.tag));
    printf("Authenticated plaintext: %s\n", recovered);
    result = 0;
done:
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(passphrase, sizeof(passphrase));
    OPENSSL_cleanse(plaintext, sizeof(plaintext));
    return result;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s rooms [ROOM_NAME]|server|signal-chat|encrypt\n", program);
    fprintf(stderr, "rooms uses named FIFOs and AES-GCM encrypted/authenticated messages.\n");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "rooms") == 0 && argc <= 3)
        return demo_rooms(argc == 3 ? argv[2] : "default");
    if (argc == 2 && strcmp(argv[1], "server") == 0) return demo_server();
    if (argc == 2 && strcmp(argv[1], "signal-chat") == 0) return demo_signal_chat();
    if (argc == 2 && strcmp(argv[1], "encrypt") == 0) return demo_encrypt();
    usage(argv[0]);
    return EXIT_FAILURE;
}

