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
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/fixed_point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

static struct list sleep_list;
static int64_t next_tick_to_awake;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

// for MLFQS
int load_avg;	// 1분 동안 수행 가능한(rady to run) 스레드의 평균 개수

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&all_list);

	// sleep_list 초기화 함수
	list_init(&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

// 가장 먼저 일어나야 할 스레드가 일어날 시각 반환
void update_next_tick_to_awake(int64_t ticks) {
	// next_tick_to_awake 변수가 깨워야 하는 스레드의 깨어날 tick 값 중 가장 작은 tick
	// 갖도록 업데이트
	next_tick_to_awake = (next_tick_to_awake > ticks) ? ticks : next_tick_to_awake;
}

// 가장 먼저 일어나야 할 스레드가 일어날 시각 반환
int64_t get_next_tick_to_awake(void) {
	return next_tick_to_awake;
}

// 스레드를 ticks 시각까지 재우는 함수
void thread_sleep(int64_t ticks) {
	struct thread *cur;

	// 인터럽트를 금지하고 이전 인터럽트 레벨을 저장함
	enum intr_level old_level;
	old_level = intr_disable();
    
	// idle 스레드는 sleep되지 않아야 함
	cur = thread_current();
	ASSERT(cur != idle_thread);

	// awake 함수가 실행되어야 할 tick 값을 update
	update_next_tick_to_awake(cur->wakeup_tick = ticks);

	// 현재 스레드를 슬립 큐에 삽입한 후 스케줄
	list_push_back(&sleep_list, &cur->elem);

	// 이 스레드를 블락하고 다시 스케줄 될 때까지 블락된 상태로 대기
	thread_block();

	// 인터럽트를 다시 받아들이도록 수정
	intr_set_level(old_level);
}

// 푹 자고 있는 스레드 중에 깨어날 시각이 ticks 시각이 지난 애들을 모조리 깨우는 함수
void thread_awake(int64_t wakeup_tick) {
	next_tick_to_awake = INT64_MAX;
	struct list_elem *e;
	e = list_begin(&sleep_list);
	while (e != list_end(&sleep_list)) {
		struct thread *t = list_entry(e, struct thread, elem);

		if (wakeup_tick >= t->wakeup_tick) {
			e = list_remove(&t->elem);
			thread_unblock(t);
		} else {
			e = list_next(e);
			update_next_tick_to_awake(t->wakeup_tick);
		}
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);

	// for MLFQS
	load_avg = LOAD_AVG_DEFAULT;
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	// 2-3 Parent child
	struct thread *cur = thread_current();
	list_push_back(&cur->child_list, &t->child_elem); // [parent] add new child to child_list

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	// 우선순위에 따른 CPU 선점하는 함수 추가
	test_max_priority();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);			// block 위해 interrupt off
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem);  // ready_list의 맨 뒤에 넣음
	// list_push_back 대신 list_insert_oerdered, thread_compare_priority 함수 이용(내림차순으로 인자 추가)
	list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, 0);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif
	list_remove(&thread_current()->allelem);
	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();

	if (curr != idle_thread) {
		// list_push_back (&ready_list, &curr->elem);	// 현 상태: list_push_back()함수로 ready list의 마지막 부분에 스레드 추가
		// list_push_back 대신 list_insert_oerdered, thread_compare_priority 함수 이용(내림차순으로 인자 추가)
		list_insert_ordered(&ready_list, &curr->elem, thread_compare_priority, 0);
	}
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority (int new_priority) {
	if (thread_mlfqs) {
		return;
	}
	thread_current ()->init_priority = new_priority;	// priority에서 init_priority로 변경

	// running 시 priority 변경 일어났을 때, donations 리스트 안에 있는 스레드들의 우선순위보다 높은 경우
	refresh_priority();

	// 우선순위에 따른 CPU 선점하는 함수 추가
	test_max_priority();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
// 현재 스레드의 nice값을 새 값으로 설정
void thread_set_nice (int nice UNUSED) {
	enum intr_level old_level = intr_disable ();
	thread_current ()->nice = nice;
	mlfqs_calculate_priority (thread_current ());
	test_max_priority ();
	intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
// 현재 스레드의 nice값을 반환
int thread_get_nice (void) {
	enum intr_level old_level = intr_disable ();
	int nice = thread_current ()-> nice;
	intr_set_level (old_level);
	return nice;
}

/* Returns 100 times the system load average. */
// 현재 시스템의 load_avg *100 값을 반환
int thread_get_load_avg (void) {
	enum intr_level old_level = intr_disable ();
	int load_avg_value = fp_to_int_round (mult_mixed (load_avg, 100));
	intr_set_level (old_level);
	return load_avg_value;
}

/* Returns 100 times the current thread's recent_cpu value. */
// 현재 스레드의 recent_cpu * 100값을 반환
int thread_get_recent_cpu (void) {
  enum intr_level old_level = intr_disable ();
  int recent_cpu= fp_to_int_round (mult_mixed (thread_current ()->recent_cpu, 100));
  intr_set_level (old_level);
  return recent_cpu;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	list_push_back(&all_list, &t->allelem);
  
	// for donation
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);

	// for MLFQS
	t->nice = NICE_DEFAULT;
  	t->recent_cpu = RECENT_CPU_DEFAULT;

	// for systeam call
	list_init(&t->child_list);
	sema_init(&t->wait_sema, 0);
	sema_init(&t->fork_sema, 0);
	sema_init(&t->free_sema, 0);

	t->running = NULL;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else  // ready_list의 맨 앞에서 꺼냄
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

void insert_sleep_list(void){
	list_push_back(&sleep_list, &thread_current()->elem);
}

// 내림차순 정렬 만드는 함수. more 리스트 인자가 less 인자보다 크면 1(true) 리턴. 반대의 경우 0(false) 리턴
bool thread_compare_priority(struct list_elem *higher, struct list_elem *lower, void *aux UNUSED) {
	return list_entry(higher, struct thread, elem)->priority > list_entry(lower, struct thread, elem)->priority;
}

// UNUSED 붙은 인자는 있어도 되고 없어도 됨.
// donations리스트에 스레드 추가 시, 내림차순으로 삽입하는 함수
bool thread_compare_donate_priority(const struct list_elem *higher, const struct list_elem *lower, void *aux UNUSED) {
	return list_entry(higher, struct thread, donation_elem)->priority > list_entry(lower, struct thread, donation_elem)->priority;
}

// priority 양도하는 함수
void donate_priority(void) {
	int depth;
	struct thread *cur = thread_current();

	for (depth = 0; depth < 8; depth++) {		// depth 8까지 왜?
		if (cur->wait_on_lock == NULL) {
			break;
		}
		else {
			struct thread *holder = cur->wait_on_lock->holder;	// 현재 lock을 가지고 있는 스레드
			holder->priority = cur->priority;
			cur = holder;
		}
	}
}

// donations 리스트에 스레드 지우는 함수
void remove_with_lock(struct lock *lock) {
	struct list_elem *e;
	struct thread *cur = thread_current();

	for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e)) {
		struct thread *t = list_entry(e, struct thread, donation_elem);
		if (t->wait_on_lock == lock) {			// wait_on_lock이 이번에 relrease하는 lock이라면
			list_remove(&t->donation_elem);		// 해당 스레드를  donation_elem 리스트에서 제거
		}
	}
}

// priority 재설정하는 함수
void refresh_priority(void) {
	struct thread *cur = thread_current();

	cur->priority = cur->init_priority;		// 현재 donation 받은 우선순위를 원래 자신의 우선순위로 바꾸기

	if (!list_empty(&cur->donations)) {		// donations 함수가 비어 있지 않다면(아직 우선순위를 줄 스레드가 있다면)
		list_sort(&cur->donations, thread_compare_donate_priority, 0);	// donations 내림차순으로 정렬(가장 큰 우선순위 맨 앞으로)

		struct thread *front = list_entry(list_front(&cur->donations), struct thread, donation_elem);
		if (front->priority > cur->priority) {
			cur->priority = front->priority;	// 가장 높은 값의 우선순위로 수정
		}
	}
}

// ready_list의 첫 번째 스레드가 현재 CPU 점유 중인 스레드보다 우선순위가 높으면 CPU 점유를 양보함
void test_max_priority(void) {
	if (!list_empty(&ready_list) && thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority)
	    thread_yield();
}

// 특정 스레드의 priority를 계산하는 함수
void mlfqs_calculate_priority(struct thread *t) {
  	if (t == idle_thread) 
    	return ;
  	t->priority = fp_to_int(add_mixed(div_mixed(t->recent_cpu, -4), PRI_MAX - t->nice * 2));
}

// 스레드의 recent_cpu 계산하는 함수
void mlfqs_calculate_recent_cpu(struct thread *t) {
  	if (t == idle_thread)
    	return ;
  	t->recent_cpu = add_mixed(mult_fp(div_fp(mult_mixed (load_avg, 2), add_mixed(mult_mixed(load_avg, 2), 1)), t->recent_cpu), t->nice);
}

// load_avg 값 계산하는 함수
void  mlfqs_calculate_load_avg(void) {
   int ready_threads;
  
  	if (thread_current() == idle_thread)	// load_avg는 스레드 고유의 값이 아니라 system wide값이기 때문에 idle_thread가 실행되는 경우도 계산
    	ready_threads = list_size(&ready_list);
  	else
    	ready_threads = list_size(&ready_list) + 1;

  	load_avg = add_fp(mult_fp(div_fp(int_to_fp(59), int_to_fp(60)), load_avg), mult_mixed(div_fp(int_to_fp(1), int_to_fp(60)), ready_threads));
}

// 현재 스레드의 recent_cpu값 +1하는 함수
void mlfqs_increment_recent_cpu(void) {
  	if (thread_current() != idle_thread)
    	thread_current()->recent_cpu = add_mixed(thread_current()->recent_cpu, 1);		// 1tick마다 running 스레드의 recent_cpu값 +1
}

// 모든 스레드의 recent_cpu값 재계산하는 함수
void mlfqs_recalculate_recent_cpu(void) {
  	struct list_elem *e;

  	for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    	struct thread *t = list_entry(e, struct thread, allelem);
    	mlfqs_calculate_recent_cpu(t);
  }
}

// 모든 스레드의 priority 재계산하는 함수
void mlfqs_recalculate_priority (void) {
  	struct list_elem *e;

  	for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    	struct thread *t = list_entry(e, struct thread, allelem);
    	mlfqs_calculate_priority (t);
  }
}