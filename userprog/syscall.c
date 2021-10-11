#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "userprog/process.h"  // process_wait 함수 가져오려면 필요
#include "filesys/filesys.h"  // filesys_create 함수 가져오려면 필요
#include "threads/palloc.h"  // palloc 관련 함수 가져오려면 필요
#include "filesys/file.h"  // file 관련 함수 가져오려면 필요

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void halt(void);
void exit(int status);
tid_t fork(const char *thread_name, struct intr_frame *f);
int exec(char *file_name);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open(const char *file);
int filesize(int fd);
void close(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	// printf ("system call!\n");

	switch (f->R.rax)
	{
	case SYS_HALT:
	    halt();
		break;
    case SYS_EXIT:
	    exit(f->R.rdi);
		break;
	case SYS_FORK:
	    f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
	    if (exec(f->R.rdi) == -1)
		    exit(-1);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi);
		break;
	case SYS_CREATE:
	    f->R.rax = create(f->R.rdi, f->R.rsi);
	    break;
	case SYS_REMOVE:
	    f->R.rax = remove(f->R.rdi);
	    break;
	case SYS_OPEN:
	    f->R.rax = open(f->R.rdi);
	    break;
	case SYS_FILESIZE:
	    f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_CLOSE:
	    close(f->R.rdi);
	    break;
	default:
		break;
	}

	// thread_exit ();
}

// Terminates Pintos by calling power_off(). No return.
void halt(void) {
	power_off();
}

// End current thread, record exit statusNo return.
void exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	printf("%s: exit(%d)\n", thread_name(), status);  // Process Termination Message
	thread_exit();
}

// (parent) Returns pid of child on success or -1 on fail
// (child) Returns 0
tid_t fork(const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}

// 다음 상황이면 바로 종료
// 1. Null pointer
// 2. A pointer to kernel virtual address space (above KERN_BASE)
// 3. A pointer to unmapped virtual memory (causes page_fault)
void check_address(const uint64_t *uaddr) {
	struct thread *cur = thread_current();
	if (uaddr == NULL || !(is_user_vaddr(uaddr)) || pml4_get_page(cur->pml4, uaddr) == NULL) {
		exit(-1);
	}
}

// Run new 'executable' from current process
// Don't confuse with open! 'open' just opens up any file(txt, excutable),
// 'exec' runs only executable
// Never returns on success. Returns -1 on fail.
int exec(char *file_name) {
    struct thread *cur = thread_current();
	check_address(file_name);

	// process_exec의 process_cleanup 때문에 f->R.rdi 날아감.
	// file_name 동적할당해서 복사한 뒤, 그걸 넘겨주기
	int siz = strlen(file_name) + 1;
	char *fn_copy = palloc_get_page(PAL_ZERO);
	if (fn_copy == NULL)
	    exit(-1);
	strlcpy(fn_copy, file_name, siz);

	if (process_exec(fn_copy) == -1)
	    return -1;

	// Not reachable
	NOT_REACHED();
	return 0;
}

// unsigned = unsigned int
bool create(const char *file, unsigned initial_size) {
	check_address(file);
	return filesys_create(file, initial_size);
}

bool remove(const char *file) {
	check_address(file);
	return filesys_remove(file);
}

// Find open spot in current thread's fdt and put file in it. Returns the fd.
int add_file_to_fdt(struct file *file) {
	struct thread *cur = thread_current();
	struct file **fdt = cur->fdTable;  // file descriptor table

	while (cur->fdIdx < FDCOUNT_LIMIT && fdt[cur->fdIdx])
	    cur->fdIdx++;
    
	// Error - fdt full
	if (cur->fdIdx >= FDCOUNT_LIMIT)
	    return -1;
	
	fdt[cur->fdIdx] = file;
	return cur->fdIdx;
}

// Opens the file called file, returns fd or -1 (if file could not be opened for some reason)
int open(const char *file) {
	check_address(file);
	struct file *fileobj = filesys_open(file);

	if (fileobj == NULL)
	    return -1;
	
	int fd = add_file_to_fdt(fileobj);

	// FD table full
	if (fd == -1)
		file_close(fileobj);

	return fd;
}

// Check if given fd is valid, return cur->fdTable[fd]
static struct file *find_file_by_fd(int fd) {
	struct thread *cur = thread_current();

	// Error - invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
	    return NULL;
	
	return cur->fdTable[fd];  // automatically returns NULL if empty
}

// Returns the size, in bytes, of the file open as fd.
int filesize(int fd) {
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj == NULL)
	    return -1;
	return file_length(fileobj);
}

void remove_file_from_fdt(int fd) {
	struct thread *cur = thread_current();

	// Error - invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
	    return;

	cur->fdTable[fd] = NULL;
}

// Closes file descriptor fd. Ignores NULL file. Returns nothing.
void close(int fd) {
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj == NULL)
	    return;

	remove_file_from_fdt(fd);
}