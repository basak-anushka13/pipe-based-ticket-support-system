#include <stdio.h>
#include <string.h>

#define main ticket_system_program_main
#include "../main.c"
#undef main

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_queue_fifo_and_capacity(void)
{
    SupportDesk desk = {0};
    int id = -1;

    CHECK(dequeue(&desk, &id) == 0);
    CHECK(enqueue(&desk, 11) == 1);
    CHECK(enqueue(&desk, 12) == 1);
    CHECK(enqueue(&desk, 13) == 1);
    CHECK(enqueue(&desk, 14) == 1);
    CHECK(enqueue(&desk, 15) == 0);
    CHECK(dequeue(&desk, &id) == 1 && id == 11);
    CHECK(enqueue(&desk, 15) == 1); /* Exercise circular wraparound. */
    CHECK(dequeue(&desk, &id) == 1 && id == 12);
    CHECK(dequeue(&desk, &id) == 1 && id == 13);
    CHECK(dequeue(&desk, &id) == 1 && id == 14);
    CHECK(dequeue(&desk, &id) == 1 && id == 15);
    CHECK(dequeue(&desk, &id) == 0);
}

static void test_command_validation_and_workflow(void)
{
    SupportDesk desk = {0};
    char out[MESSAGE_SIZE];
    desk.next_id = 1;

    handle_command(&desk, "/help", out, sizeof(out));
    CHECK(strstr(out, "/new CUSTOMER") != NULL);
    handle_command(&desk, "/new invalid", out, sizeof(out));
    CHECK(strstr(out, "Usage:") != NULL);
    CHECK(desk.count == 0);

    handle_command(&desk, "/new Alice|Cannot log in|3", out, sizeof(out));
    CHECK(strstr(out, "Created and queued ticket #1") != NULL);
    CHECK(desk.count == 1 && desk.queued == 1);
    handle_command(&desk, "/stats", out, sizeof(out));
    CHECK(strstr(out, "total=1 queued=1/4") != NULL);
    handle_command(&desk, "/process", out, sizeof(out));
    CHECK(strstr(out, "status: Escalated") != NULL);

    handle_command(&desk, "/list", out, sizeof(out));
    CHECK(strstr(out, "Alice") != NULL && strstr(out, "Escalated") != NULL);
    handle_command(&desk, "/queue", out, sizeof(out));
    CHECK(strstr(out, "Moved 0 waiting") != NULL);
    handle_command(&desk, "/quit", out, sizeof(out));
    CHECK(strstr(out, "shutting down") != NULL);
    handle_command(&desk, "/unknown", out, sizeof(out));
    CHECK(strstr(out, "Unknown command") != NULL);
}

int main(void)
{
    test_queue_fifo_and_capacity();
    test_command_validation_and_workflow();
    if (failures != 0) {
        (void)fprintf(stderr, "%d unit check(s) failed\n", failures);
        return 1;
    }
    (void)puts("Native C unit tests passed.");
    return 0;
}
