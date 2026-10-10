/* FreeRTOS subset (see freertos/*.h) on Zephyr kernel objects. */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/event_groups.h"

/* ---- tasks ---- */

struct frt_task {
    struct k_thread   thread;
    void             *stack;
    TaskFunction_t    fn;
    void             *arg;
    atomic_t          value;
    struct k_sem      notify;
    struct frt_task  *next_dead;
};

static struct k_spinlock s_dead_lock;
static struct frt_task  *s_dead;

static void reap(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_dead_lock);
    struct frt_task **pp = &s_dead;
    while (*pp) {
        struct frt_task *t = *pp;
        if (k_thread_join(&t->thread, K_NO_WAIT) == 0) {
            *pp = t->next_dead;
            free(t->stack);
            free(t);
        } else {
            pp = &t->next_dead;
        }
    }
    k_spin_unlock(&s_dead_lock, key);
}

static void entry(void *a, void *b, void *c)
{
    struct frt_task *t = a;
    t->fn(t->arg);
    vTaskDelete(NULL);           /* a FreeRTOS task must not return, but be safe */
}

/* FreeRTOS: bigger number = more urgent.  Zephyr: smaller = more urgent.
 * Tasks 1..24 map onto preemptible priorities 14..1. */
static int map_prio(UBaseType_t p)
{
    int z = 14 - (int)p;
    if (z < 1) z = 1;
    if (z > 14) z = 14;
    return z;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                       void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    reap();
    struct frt_task *t = calloc(1, sizeof(*t));
    if (!t) return pdFAIL;
    /* Stacks come from the C heap: the kernel heap is shared with the
     * Wi-Fi driver and much smaller. */
    t->stack = aligned_alloc(Z_KERNEL_STACK_OBJ_ALIGN,
                             ROUND_UP(K_KERNEL_STACK_LEN(stack_bytes), Z_KERNEL_STACK_OBJ_ALIGN));
    if (!t->stack) { free(t); return pdFAIL; }
    t->fn = fn;
    t->arg = arg;
    k_sem_init(&t->notify, 0, 1);
    if (out) *out = t;
    k_tid_t tid = k_thread_create(&t->thread, (k_thread_stack_t *)t->stack, stack_bytes,
                                  entry, t, NULL, NULL, map_prio(prio), 0, K_NO_WAIT);
    if (name) k_thread_name_set(tid, name);
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    /* Only tasks made by xTaskCreate have a handle; the thread struct is first. */
    return (TaskHandle_t)k_current_get();
}

void vTaskDelete(TaskHandle_t t)
{
    bool self = !t || &t->thread == k_current_get();
    if (!t) t = (struct frt_task *)k_current_get();
    k_spinlock_key_t key = k_spin_lock(&s_dead_lock);
    t->next_dead = s_dead;
    s_dead = t;
    k_spin_unlock(&s_dead_lock, key);
    if (self) {
        k_thread_abort(k_current_get());
    } else {
        k_thread_abort(&t->thread);
        reap();
    }
}

BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action)
{
    if (!t) return pdFAIL;
    switch (action) {
    case eSetBits:                   atomic_or(&t->value, value); break;
    case eIncrement:                 atomic_inc(&t->value); break;
    case eSetValueWithOverwrite:
    case eSetValueWithoutOverwrite:  atomic_set(&t->value, value); break;
    default: break;
    }
    k_sem_give(&t->notify);
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *value, TickType_t wait)
{
    struct frt_task *t = (struct frt_task *)k_current_get();
    if (k_sem_count_get(&t->notify) == 0) atomic_and(&t->value, ~clear_entry);
    if (k_sem_take(&t->notify, frt_timeout(wait)) != 0) return pdFALSE;
    uint32_t v = atomic_and(&t->value, ~clear_exit);
    if (value) *value = v;
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    struct frt_task *t = (struct frt_task *)k_current_get();
    if (k_sem_take(&t->notify, frt_timeout(wait)) != 0) return 0;
    uint32_t v;
    if (clear) {
        v = atomic_set(&t->value, 0);
    } else {
        v = atomic_dec(&t->value);
        if (v > 1) k_sem_give(&t->notify);
    }
    return v;
}

/* ---- semaphores and mutexes ---- */

SemaphoreHandle_t frt_sem_init(StaticSemaphore_t *s, bool mutex, unsigned max, unsigned initial)
{
    if (!s) s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->is_mutex = mutex;
    if (mutex) k_mutex_init(&s->mutex);
    else       k_sem_init(&s->sem, initial, max);
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    int r = s->is_mutex ? k_mutex_lock(&s->mutex, frt_timeout(wait))
                        : k_sem_take(&s->sem, frt_timeout(wait));
    return r == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s->is_mutex) return k_mutex_unlock(&s->mutex) == 0 ? pdTRUE : pdFALSE;
    k_sem_give(&s->sem);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }

/* ---- queues ---- */

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    struct k_msgq *q = malloc(sizeof(*q) + (size_t)len * item_size);
    if (!q) return NULL;
    k_msgq_init(q, (char *)(q + 1), item_size, len);
    return q;
}

void vQueueDelete(QueueHandle_t q) { free(q); }

/* ---- stream buffers ---- */

struct frt_stream {
    struct ring_buf rb;
    struct k_mutex  lock;
    struct k_sem    data;      /* given whenever bytes are added */
    struct k_sem    space;     /* given whenever bytes are removed */
    size_t          trigger;
    uint8_t         buf[];
};

StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger)
{
    struct frt_stream *s = malloc(sizeof(*s) + size);
    if (!s) return NULL;
    ring_buf_init(&s->rb, size, s->buf);
    k_mutex_init(&s->lock);
    k_sem_init(&s->data, 0, 1);
    k_sem_init(&s->space, 0, 1);
    s->trigger = trigger ? trigger : 1;
    return s;
}

void vStreamBufferDelete(StreamBufferHandle_t s) { free(s); }

size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t wait)
{
    const uint8_t *p = data;
    size_t done = 0;
    int64_t end = wait == portMAX_DELAY ? INT64_MAX : k_uptime_get() + wait;
    for (;;) {
        k_mutex_lock(&s->lock, K_FOREVER);
        done += ring_buf_put(&s->rb, p + done, len - done);
        k_mutex_unlock(&s->lock);
        if (done) k_sem_give(&s->data);
        if (done == len) break;
        int64_t left = end - k_uptime_get();
        if (left <= 0 || k_sem_take(&s->space, wait == portMAX_DELAY ? K_FOREVER : K_MSEC(left)) != 0) break;
    }
    return done;
}

size_t xStreamBufferReceive(StreamBufferHandle_t s, void *buf, size_t len, TickType_t wait)
{
    int64_t end = wait == portMAX_DELAY ? INT64_MAX : k_uptime_get() + wait;
    for (;;) {
        k_mutex_lock(&s->lock, K_FOREVER);
        size_t avail = ring_buf_size_get(&s->rb);
        size_t n = 0;
        if (avail >= s->trigger || (avail && avail >= len)) n = ring_buf_get(&s->rb, buf, len);
        k_mutex_unlock(&s->lock);
        if (n) { k_sem_give(&s->space); return n; }
        int64_t left = end - k_uptime_get();
        if (left <= 0 || k_sem_take(&s->data, wait == portMAX_DELAY ? K_FOREVER : K_MSEC(left)) != 0) {
            /* timed out: hand back whatever is there, like FreeRTOS does */
            k_mutex_lock(&s->lock, K_FOREVER);
            n = ring_buf_get(&s->rb, buf, len);
            k_mutex_unlock(&s->lock);
            if (n) k_sem_give(&s->space);
            return n;
        }
    }
}

BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t s)
{
    return ring_buf_is_empty(&s->rb) ? pdTRUE : pdFALSE;
}

BaseType_t xStreamBufferReset(StreamBufferHandle_t s)
{
    k_mutex_lock(&s->lock, K_FOREVER);
    ring_buf_reset(&s->rb);
    k_mutex_unlock(&s->lock);
    return pdPASS;
}

/* ---- event groups ---- */

EventGroupHandle_t xEventGroupCreate(void)
{
    struct k_event *e = malloc(sizeof(*e));
    if (e) k_event_init(e);
    return e;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait)
{
    uint32_t got = all ? k_event_wait_all(e, bits, false, frt_timeout(wait))
                       : k_event_wait(e, bits, false, frt_timeout(wait));
    EventBits_t now = k_event_test(e, ~0u);
    if (got && clear) k_event_clear(e, bits);
    return now;
}
