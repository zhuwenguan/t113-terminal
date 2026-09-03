#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_receiver.h"

#include "test_check.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

static void test_active_mask_lifecycle(void)
{
    CanReceiver receiver;

    memset(&receiver, 0, sizeof(receiver));
    receiver.epoll_fd = 1;
    receiver.socket_fd[0] = 10;
    receiver.socket_fd[1] = 11;
    receiver.active_mask = 0x3U;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0x3U);

    receiver.stats[0].interface_failed = true;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0x2U);
    receiver.socket_fd[0] = -1;
    receiver.active_mask &= ~0x1U;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0x2U);

    receiver.stats[1].interface_failed = true;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0U);
    receiver.socket_fd[1] = -1;
    receiver.active_mask &= ~0x2U;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0U);

    receiver.socket_fd[1] = 12;
    receiver.stats[1].interface_failed = false;
    receiver.active_mask |= 0x2U;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0x2U);

    receiver.socket_fd[0] = 13;
    receiver.stats[0].interface_failed = false;
    receiver.active_mask |= 0x1U;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0x3U);

    receiver.epoll_fd = -1;
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0U);
    TEST_CHECK(can_receiver_active_mask(NULL) == 0U);
}

static void *run_receiver(void *opaque)
{
    CanReceiver *receiver = opaque;

    TEST_CHECK(can_receiver_run(receiver) == 0);
    return NULL;
}

static void test_reconnect_is_rate_limited(void)
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "t113miss0",
        "t113miss1"
    };
    CanReceiver receiver;
    pthread_t thread;

    TEST_CHECK(can_receiver_init(&receiver, interfaces) == 0);
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0U);
    TEST_CHECK(receiver.stats[0].interface_failed);
    TEST_CHECK(receiver.stats[1].interface_failed);
    TEST_CHECK(pthread_create(&thread, NULL, run_receiver, &receiver) == 0);

    /* The initial failure schedules the first retry one second later.  Run
     * long enough for two attempts, but not long enough for a busy loop to
     * be mistaken for a successful interval. */
    (void)sleep(2U);
    can_receiver_stop(&receiver);
    TEST_CHECK(pthread_join(thread, NULL) == 0);
    TEST_CHECK(receiver.reconnect_attempts[0] >= 1U);
    TEST_CHECK(receiver.reconnect_attempts[0] <= 3U);
    TEST_CHECK(receiver.reconnect_attempts[1] >= 1U);
    TEST_CHECK(receiver.reconnect_attempts[1] <= 3U);
    TEST_CHECK(can_receiver_active_mask(&receiver) == 0U);
    can_receiver_close(&receiver);
}

int main(void)
{
    test_active_mask_lifecycle();
    test_reconnect_is_rate_limited();
    return 0;
}
