#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed-point.h"


#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Fixed-point formatinda sistem yuk ortalamasi */
int load_avg;

#define THREAD_MAGIC 0xcd6abf4b

static struct list ready_list;
static struct list all_list;

struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

struct kernel_thread_frame {
    void *eip;
    thread_func *function;
    void *aux;
};

/* Gelişmiş Zamanlayıcı Prototipleri */
void mlfqs_calculate_priority (struct thread *t, void *aux);
void mlfqs_calculate_all_priorities (void);

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

void
thread_init (void) {
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&all_list);

    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();

    if (thread_mlfqs) {
        load_avg = 0;
        initial_thread->nice = 0;
        initial_thread->recent_cpu = 0;
    }
}

void
thread_start (void) {
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    intr_enable ();
    sema_down (&idle_started);
}

void
thread_tick (void) {
    struct thread *t = thread_current ();

    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return ();
}

void
thread_print_stats (void) {
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) {
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT (function != NULL);

    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    sf = alloc_frame (t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    if (thread_mlfqs) {
        t->nice = thread_current ()->nice;
        t->recent_cpu = thread_current ()->recent_cpu;
    }

    thread_unblock (t);
    return tid;
}

void
thread_block (void) {
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);

    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

void
thread_unblock (struct thread *t) {
    enum intr_level old_level;

    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);
    list_push_back (&ready_list, &t->elem);
    t->status = THREAD_READY;
    intr_set_level (old_level);
}

const char *
thread_name (void) {
    return thread_current ()->name;
}

struct thread *
thread_current (void) {
    struct thread *t = running_thread ();
    ASSERT (is_thread (t));
    ASSERT (t->status == THREAD_RUNNING);
    return t;
}

tid_t
thread_tid (void) {
    return thread_current ()->tid;
}

void
thread_exit (void) {
    ASSERT (!intr_context ());
#ifdef USERPROG
    process_exit ();
#endif
    intr_disable ();
    list_remove (&thread_current()->allelem);
    thread_current ()->status = THREAD_DYING;
    schedule ();
    NOT_REACHED ();
}

void
thread_yield (void) {
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread)
        list_push_back (&ready_list, &cur->elem);
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

void
thread_foreach (thread_action_func *func, void *aux) {
    struct list_elem *e;
    ASSERT (intr_get_level () == INTR_OFF);

    for (e = list_begin (&all_list); e != list_end (&all_list);
         e = list_next (e)) {
        struct thread *t = list_entry (e, struct thread, allelem);
        func (t, aux);
    }
}

void
thread_set_priority (int new_priority) {
    if (thread_mlfqs)
        return;
    thread_current ()->priority = new_priority;
}

int
thread_get_priority (void) {
    return thread_current ()->priority;
}

void
thread_set_nice (int new_nice) {
    if (!thread_mlfqs)
        return;

    enum intr_level old_level = intr_disable ();
    thread_current ()->nice = new_nice;
    mlfqs_calculate_priority (thread_current (), NULL);
    intr_set_level (old_level);
    thread_yield ();
}

int
thread_get_nice (void) {
    enum intr_level old_level = intr_disable ();
    int current_nice = thread_current ()->nice;
    intr_set_level (old_level);
    return current_nice;
}

int
thread_get_load_avg (void) {
    enum intr_level old_level = intr_disable ();
    long long scaled = (long long) load_avg * 100;
    int rounded_load_avg;

    if (scaled >= 0)
        rounded_load_avg = (scaled + (1 << 13)) / (1 << 14);
    else
        rounded_load_avg = (scaled - (1 << 13)) / (1 << 14);

    intr_set_level (old_level);
    return rounded_load_avg;
}

int
thread_get_recent_cpu (void) {
    enum intr_level old_level = intr_disable ();
    long long scaled = (long long) thread_current ()->recent_cpu * 100;
    int rounded_recent_cpu;

    if (scaled >= 0)
        rounded_recent_cpu = (scaled + (1 << 13)) / (1 << 14);
    else
        rounded_recent_cpu = (scaled - (1 << 13)) / (1 << 14);

    intr_set_level (old_level);
    return rounded_recent_cpu;
}

static void
idle (void *idle_started_ UNUSED) {
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current ();
    sema_up (idle_started);

    for (;;) {
        intr_disable ();
        thread_block ();
        asm volatile ("sti; hlt" : : : "memory");
    }
}

static void
kernel_thread (thread_func *function, void *aux) {
    ASSERT (function != NULL);
    intr_enable ();
    function (aux);
    thread_exit ();
}

struct thread *
running_thread (void) {
    uint32_t *esp;
    asm ("mov %%esp, %0" : "=g" (esp));
    return pg_round_down (esp);
}

static bool
is_thread (struct thread *t) {
    return t != NULL && t->magic == THREAD_MAGIC;
}

static void
init_thread (struct thread *t, const char *name, int priority) {
    enum intr_level old_level;
    ASSERT (t != NULL);
    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT (name != NULL);

    memset (t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy (t->name, name, sizeof t->name);
    t->stack = (uint8_t *) t + PGSIZE;
    t->priority = priority;
    t->magic = THREAD_MAGIC;

    if (thread_mlfqs) {
        int fp_recent_div_4 = t->recent_cpu / 4;
        int int_recent_div_4 = fp_recent_div_4 / (1 << 14);
        int new_priority = PRI_MAX - int_recent_div_4 - (t->nice * 2);

        if (new_priority > PRI_MAX) new_priority = PRI_MAX;
        if (new_priority < PRI_MIN) new_priority = PRI_MIN;

        t->priority = new_priority;
    }

    old_level = intr_disable ();
    list_push_back (&all_list, &t->allelem);
    intr_set_level (old_level);
}

static void *
alloc_frame (struct thread *t, size_t size) {
    ASSERT (is_thread (t));
    ASSERT (size % sizeof (uint32_t) == 0);
    t->stack -= size;
    return t->stack;
}

static struct thread *
next_thread_to_run (void) {
    if (list_empty (&ready_list))
        return idle_thread;
    else
        return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

void
thread_schedule_tail (struct thread *prev) {
    struct thread *cur = running_thread ();
    ASSERT (intr_get_level () == INTR_OFF);
    cur->status = THREAD_RUNNING;
    thread_ticks = 0;
#ifdef USERPROG
    process_activate ();
#endif
    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
        ASSERT (prev != cur);
        palloc_free_page (prev);
    }
}

static void
schedule (void) {
    struct thread *cur = running_thread ();
    struct thread *next = next_thread_to_run ();
    struct thread *prev = NULL;

    ASSERT (intr_get_level () == INTR_OFF);
    ASSERT (cur->status != THREAD_RUNNING);
    ASSERT (is_thread (next));

    if (cur != next)
        prev = switch_threads (cur, next);
    thread_schedule_tail (prev);
}

static tid_t
allocate_tid (void) {
    static tid_t next_tid = 1;
    tid_t tid;
    lock_acquire (&tid_lock);
    tid = next_tid++;
    lock_release (&tid_lock);
    return tid;
}

void
mlfqs_calculate_priority (struct thread *t, void *aux UNUSED) {
    if (t == idle_thread)
        return;

    int recent_div_4 = DIV_MIXED (t->recent_cpu, 4);
    int nice_mult_2 = CONVERT_N_TO_FP (t->nice * 2);
    int priority = SUB_FP (CONVERT_N_TO_FP (PRI_MAX), ADD_FP (recent_div_4, nice_mult_2));

    t->priority = CONVERT_FP_TO_INT_ROUND (priority);

    if (t->priority > PRI_MAX) t->priority = PRI_MAX;
    if (t->priority < PRI_MIN) t->priority = PRI_MIN;
}

void
mlfqs_calculate_all_priorities (void) {
    thread_foreach (mlfqs_calculate_priority, NULL);
}

void
mlfqs_update_load_avg (void) {
    int ready_threads = list_size (&ready_list);
    if (thread_current () != idle_thread)
        ready_threads++;

    int term1 = MUL_FP (DIV_FP (CONVERT_N_TO_FP (59), CONVERT_N_TO_FP (60)), load_avg);
    int term2 = MUL_MIXED (DIV_FP (CONVERT_N_TO_FP (1), CONVERT_N_TO_FP (60)), ready_threads);

    load_avg = ADD_FP (term1, term2);
}

void
mlfqs_update_recent_cpu (struct thread *t, void *aux UNUSED) {
    if (t != idle_thread) {
        int load_2 = MUL_MIXED (load_avg, 2);
        int coef = DIV_FP (load_2, ADD_MIXED (load_2, 1));
        t->recent_cpu = ADD_MIXED (MUL_FP (coef, t->recent_cpu), t->nice);
    }
}
