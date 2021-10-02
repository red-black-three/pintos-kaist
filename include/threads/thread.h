#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          // Thread identifier. 정수형. 프로세스에 1부터 부여. 최신 프로세스일수록 높은 숫자
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       // 0~63. 숫자 낮을수록 우선순위 낮음.

	// 스레드가 priority를 양도받았다가 다시 반납할 때 원래의 priority를 복원할 수
	// 있도록 고유의 priority 값을 저장하는 변수
	int init_priority;

	int64_t wakeup_tick;  // 깨어나야 할 tick을 저장할 변수 추가

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

	// 스레드가 현재 얻기 위해 기다리고 있는 lock.
	// 스레드는 이 lock이 ralease 되기를 기다림
	struct lock *wait_on_lock;

	// 이 스레드에게 priority를 나눠준 스레드들의 리스트
	struct list donations;

	// 위 리스트를 관리하기 위한 element. 위에 elem과 구분하여 사용.
	struct list_elem donation_elem;

	int64_t sleep_ticks;

	// donation 위한 변수 추가
	int init_priority;					// thread의 원래 priority. 양도 받은 priority 반납 후 복원할 때 필요
	struct lock *wait_on_lock;			// 스레드가 얻으려고 하는 lock
	struct list donations;				// 자신에게 priority 양도해준 스레드들 리스트
	struct list_elem donation_elem;		// donations 관리 위한 elem

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               // Information for switching(레지스터, 스택 포인터 포함)
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */

// 스레드를 ticks 시각까지 재우는 함수
void thread_sleep(int64_t ticks);

// 푹 자고 있는 스레드 중에 깨어날 시각이 ticks 시각이 지난 애들을 모조리 깨우는 함수
void thread_awake(int64_t ticks);

// 가장 먼저 일어나야 할 스레드가 일어날 시각을 반환함
int64_t get_next_tick_to_awake(void);

// 가장 먼저 일어날 스레드가 일어날 시각을 업데이트함
void update_next_tick_to_awake(int64_t ticks);


// 우선순위 비교 함수 선언
bool thread_compare_priority(struct list_elem *higher, struct list_elem *lower, void *aux UNUSED);
bool sema_compare_priority(const struct list_elem *higher, const struct list_elem *lower, void *aux UNUSED);
bool thread_compare_donate_priority(const struct list_elem *higher, const struct list_elem *lower, void *aux UNUSED);

void donate_priority(void);

void remove_with_lock(struct lock *lock);

void refresh_priority(void);

void test_max_priority(void);