#include "../kernel/cgrtos.h"

/*
 * Interactive demo — built with `make APP=demo && make run`.
 * Relies on the same tick path validated by `make test`.
 *
 * Includes a SCHED_EDF periodic task to exercise MC-EDF alongside
 * classic priority producer/consumer.
 */

static cgrtos_sem_t *sem;
static cgrtos_queue_t *q;
static cgrtos_event_group_t *eg;
static volatile uint32_t blinks;

static void timer_cb(void *arg)
{
    (void)arg;
    cgrtos_printf("  [DEMO] soft-timer tick=%lu\n", (unsigned long)cgrtos_get_ticks());
}

static void producer(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    cgrtos_timer_t *tmr = cgrtos_timer_create("demo", timer_cb, 0,
                                              portMS_TO_TICK(400), 1);
    if (tmr) {
        cgrtos_timer_start(tmr);
    }

    while (1) {
        n++;
        blinks++;
        cgrtos_printf("  [DEMO] produce %u @%lu\n", n, (unsigned long)cgrtos_get_ticks());
        cgrtos_event_group_set(eg, 0x1);
        cgrtos_queue_send(q, &n, portMAX_DELAY);
        cgrtos_sem_give(sem);
        cgrtos_delay_ms(250);
    }
}

static void consumer(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_sem_take(sem, portMAX_DELAY);
        event_flags_t f = cgrtos_event_group_wait_bits(eg, 0x1, 1, 0, portMAX_DELAY);
        uint32_t v = 0;
        cgrtos_queue_receive(q, &v, portMAX_DELAY);
        cgrtos_printf("  [DEMO] consume %u flags=0x%x blinks=%u\n",
                      v, f, blinks);
    }
}

static void ticker(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_delay_ms(1000);
        cgrtos_printf("  [DEMO] heartbeat ticks=%lu\n",
                      (unsigned long)cgrtos_get_ticks());
    }
}

static void edf_beat(void *arg)
{
    (void)arg;
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *self = g_current[cpu];
    tick_t period = portMS_TO_TICK(100);
    tick_t wake = cgrtos_get_ticks() + period;

    if (self) {
        cgrtos_task_set_period(self->id, period);
        cgrtos_task_set_deadline(self->id, wake);
    }

    while (1) {
        tick_t now = cgrtos_get_ticks();
        if (now < wake) {
            cgrtos_delay_until(&wake, period);
        } else {
            wake = now + period;
            if (self) {
                cgrtos_task_set_deadline(self->id, wake);
            }
        }
        cpu = (uint8_t)read_csr(mhartid);
        self = g_current[cpu];
        cgrtos_printf("  [DEMO][MC-EDF] beat @%lu dl=%lu cpu=%u\n",
                      (unsigned long)cgrtos_get_ticks(),
                      (unsigned long)(self ? self->deadline : 0),
                      (unsigned)cpu);
    }
}

int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;

    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d — CG-RTOS demo (DDR end %p)\n", hartid, end);

    sem = cgrtos_sem_create(0, 8);
    q = cgrtos_queue_create(4, sizeof(uint32_t));
    eg = cgrtos_event_group_create();

    cgrtos_task_create("producer", producer, 0, 8, SCHED_PRIORITY);
    cgrtos_task_create("consumer", consumer, 0, 7, SCHED_PRIORITY);
    cgrtos_task_create("ticker", ticker, 0, 3, SCHED_RR);
    cgrtos_task_create("edfbeat", edf_beat, 0, 4, SCHED_EDF);

    cgrtos_start();
    return 0;
}

void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
