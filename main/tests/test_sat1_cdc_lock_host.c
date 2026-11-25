/* Compile the actual CDC client callbacks with a contended RPC boundary. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define appconfUSB_CDC_ENABLED 1
#define xassert assert
#define RTOS_OSAL_WAIT_FOREVER ((unsigned)-1)
#define RTOS_OSAL_NOT_RECURSIVE 0
#define RPC_PARAM_IN_BUFFER(buffer, length) 0
#define RPC_PARAM_TYPE(value) 0
#define RPC_PARAM_LIST_END 0
typedef int rpc_param_desc_t;
typedef pthread_mutex_t rtos_osal_mutex_t;
typedef struct { int unused; } rtos_intertile_t;
typedef struct { int port; rtos_intertile_t *intertile_ctx; } rtos_intertile_address_t;
typedef struct {
    void *rpc_host_start;
    int remote_client_count, host_task_priority;
    rtos_intertile_address_t host_address;
} rtos_driver_rpc_t;
static rtos_driver_rpc_t rpc_config_s;
static rtos_driver_rpc_t *rpc_config = &rpc_config_s;
static rtos_osal_mutex_t client_print_lock;
enum { fcode_rx, fcode_tx };
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned active_calls, maximum_active_calls, total_calls;

static __attribute__((unused)) void rtos_osal_mutex_create(rtos_osal_mutex_t *mutex, char *name, int recursive)
{
    (void)name;
    assert(recursive == 0 && pthread_mutex_init(mutex, NULL) == 0);
}
static __attribute__((unused)) void rtos_osal_mutex_get(rtos_osal_mutex_t *mutex, unsigned wait)
{
    (void)wait;
    assert(pthread_mutex_lock(mutex) == 0);
}
static __attribute__((unused)) void rtos_osal_mutex_put(rtos_osal_mutex_t *mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
}
static int tud_cdc_connected(void) { return 1; }
static void tud_cdc_write(const void *data, size_t bytes) {(void)data;(void)bytes;}
static void tud_cdc_write_flush(void) {}

static void rpc_client_call_generic(rtos_intertile_t *ctx, unsigned port,
                                     int function, const rpc_param_desc_t *desc, ...)
{
    (void)ctx;
    (void)desc;
    assert(port == 3 && function == fcode_tx);
    va_list args;
    va_start(args, desc);
    typedef char buffer_t[256];
    buffer_t *buffer = va_arg(args, buffer_t *);
    int *length = va_arg(args, int *);
    assert(*length > 0 && *length < 256 && (*buffer)[*length] == 0);
    va_end(args);
    assert(pthread_mutex_lock(&counter_lock) == 0);
    ++active_calls;
    ++total_calls;
    if (active_calls > maximum_active_calls) maximum_active_calls = active_calls;
    assert(pthread_mutex_unlock(&counter_lock) == 0);
    /* Keep the request/response transaction open while other writers run. */
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&delay, NULL);
    assert(pthread_mutex_lock(&counter_lock) == 0);
    --active_calls;
    assert(pthread_mutex_unlock(&counter_lock) == 0);
}

#include "sat1_cdc_callbacks.inc"

static void *writer(void *arg)
{
    for (unsigned i = 0; i < 20; ++i) {
        assert(cdc_printf("writer=%u count=%u", (unsigned)(uintptr_t)arg, i) > 0);
    }
    return NULL;
}

int main(void)
{
    rtos_intertile_t link;
    rtos_cdc_rpc_client_init(&link);
    rpc_config->host_address.port = 3;
    pthread_t writers[8];
    for (unsigned i = 0; i < 8; ++i) {
        assert(pthread_create(&writers[i], NULL, writer, (void *)(uintptr_t)i) == 0);
    }
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(writers[i], NULL) == 0);
    assert(maximum_active_calls == 1 && total_calls == 160);
    char oversized[600];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = 0;
    assert(cdc_printf("%s", oversized) == 255);
    assert(cdc_printf("") == 0 && total_calls == 161);
    assert(pthread_mutex_destroy(&client_print_lock) == 0);
    return 0;
}
