#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

#include "threads/synch.h"
#include "userprog/syscall.h"

#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
// static void
void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	char *token;
	char *save_ptr;
	// printf("f_name1: %s\n" ,*(&file_name));

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL){
		// palloc_free_page (fn_copy);
		return TID_ERROR;
	}

	strlcpy (fn_copy, file_name, PGSIZE);
	token = strtok_r(file_name, " ", &save_ptr);
	// printf("f_name2: %s\n" ,*(&file_name));
	
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	// printf("tid = %d\n",tid);

	if (tid == TID_ERROR){
		palloc_free_page (fn_copy);
	}

	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
	// printf("initd\n");
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	// printf("initd1\n");
	process_init ();
	// printf("initd2\n");
	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	// printf("initd3\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	struct thread *curr = thread_current();

	// struct intr_frame *f = (pg_round_up(rrsp()) - sizeof(struct intr_frame)); //(oom_update)
	// memcpy(&parent->syscall_tf, f, sizeof(struct intr_frame));
	memcpy (&curr->syscall_tf, if_, sizeof(struct intr_frame));

	tid_t child_tid = thread_create(name, PRI_DEFAULT, __do_fork, (void *)curr);
	if (child_tid == TID_ERROR) {
		return TID_ERROR;
	}

	struct thread *child = get_thread_by_tid(child_tid);
	if (child == NULL) {
		return TID_ERROR;
	}

	sema_down(&child->fork_sema);

	if (child->process_status == PROCESS_ERR) {
		sema_up(&child->free_sema);		//이거 필요한지 확실하지 않음 (oom_update)
		return TID_ERROR;
	}

	return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va)) {
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	if ((parent_page = pml4_get_page (parent->pml4, va)) == NULL) {
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	if ((newpage = palloc_get_page(PAL_USER | PAL_ZERO)) == NULL) {
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);  
		return false;
	}
	
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *curr = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = &parent->syscall_tf;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;			// 자식 프로세스는 fork() 호출 후 항상 0을 반환

	/* 2. Duplicate PT */
	curr->pml4 = pml4_create();
	if (curr->pml4 == NULL)
		goto error;

	process_activate (curr);
#ifdef VM
	supplemental_page_table_init (&curr->spt);
	if (!supplemental_page_table_copy (&curr->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	for (int i=3; i<FD_MAX; i++) {
		if (parent->fd_table[i] != NULL){
			curr->fd_table[i] = file_duplicate(parent->fd_table[i]);
		}
	}
	curr->next_fd = parent->next_fd;
	sema_up(&curr->fork_sema);
	process_init ();

	if (succ)
		do_iret (&if_);
error:
	sema_up(&curr->fork_sema);
	exit (-1);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
// 단순히 프로그램 파일 이름만을 인자로 받아오게 하는 대신
// 공백을 기준으로 여러 단어로 나누어지게 만드세요.
int
process_exec (void *f_name) {  
	char *file_name = f_name;	
	bool success;
	// printf("f_name3: %s\n" ,*(&f_name));
	// printf("finame: %p\n" ,file_name);
	
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	lock_acquire(&syscall_lock); //minjae's
	success = load (file_name, &_if);
	lock_release(&syscall_lock); //minjae's

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	
	struct thread *child = NULL;                 // 자식 스레드를 저장할 변수
	if ((child = get_thread_by_tid(child_tid)) == NULL || child < 0) {	//(oom_update)
		return -1;
	}
	sema_down(&child->wait_sema);
	int child_status = child->process_status;

	list_remove(&child->child_elem);
	sema_up(&child->free_sema);

	return child_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	for (int i = 3; i <= FD_MAX; i++) {		//(oom_update)
		close(i);
	}
	palloc_free_multiple(curr->fd_table, FD_PAGES);
	file_close(curr->running); //minjae's
	process_cleanup();

	sema_up(&curr->wait_sema); // 끝나고 기다리는 부모한테 세마포 넘겨줌
	sema_down(&curr->free_sema); // 부모가 자식 free하고 세마포 넘길 때까지 기다림
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void 
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);

static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	
	char *args[32];
	int argc = 0;
	char *save_ptr;
	// char fn_copy[64];
	char *fn_copy;
	
	fn_copy = palloc_get_page(PAL_ZERO);
	if (fn_copy == NULL) {
		goto done;
	}

	// 문자열 파싱
    strlcpy(fn_copy, file_name, PGSIZE);
	for (char *token = strtok_r(fn_copy, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)) {
    	args[argc++] = token;
	}
	
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (args[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", args[0]);
		goto done;
	}

	t->running = file;			// minjae's
	file_deny_write(file);		// minjae's

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	for(int i = argc-1 ; i>=0;i--){
		size_t arg_len = strlen(args[i]) + 1;
		if_->rsp -= arg_len;
		memcpy(if_->rsp,args[i],arg_len);
		args[i] = if_->rsp;
	}
	int mod = if_->rsp % 8;  // 8바이트 정렬을 위해 나머지 계산
	if (mod != 0) {
		if_->rsp -= mod;
		uint8_t word_align[8] = {0};
		memcpy(if_->rsp, word_align, mod);
	}
	if_->rsp -= sizeof(char *);
	memset(if_->rsp, 0, sizeof(char *));
	for (int i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof(char *);
		memcpy(if_->rsp, &args[i], sizeof(char *));  
	}

	char **argv = if_->rsp;

	if_->rsp -= sizeof(void *);
    *(void **)if_->rsp = NULL;

	if_->rsp = (uint64_t)if_->rsp;
	// printf("\n=======\n%p\n=======\n\n", if_->rsp);

	if_->R.rdi = argc;
	if_->R.rsi = argv;

	// hex_dump((uintptr_t)if_->rsp, (void *)if_->rsp, USER_STACK - (uintptr_t)if_->rsp, true);
	success = true;


done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);		// minjae's 경우 없앰
	// if (fn_copy != NULL) {
    //     palloc_free_page(fn_copy);
    // }
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		ofs += page_read_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else // (다 구현하면 이거 주석처리 풀자~)
/* 여기서부터는 project 3 이후에 사용되는 코드입니다.
 * project 2에만 해당하는 기능을 구현하려면, 상위 블록에서 구현하세요. */

bool
lazy_load_segment (struct page *page, void *aux) {
	/* aux를 lazy_load_arg 구조체로 캐스팅하여 파일과 오프셋 정보를 가져옵니다. */
	struct lazy_load_arg *lazy_load_arg = (struct lazy_load_arg *)aux;
	
	/* 파일에서 필요한 오프셋으로 이동합니다. */
	file_seek(lazy_load_arg->file, lazy_load_arg->ofs);

	/* 파일에서 지정된 바이트 수를 읽어옵니다. */
	size_t bytes_read = file_read(lazy_load_arg->file, page->frame->kva, lazy_load_arg->page_read_bytes);
	
	if (bytes_read != lazy_load_arg->page_read_bytes) {
		palloc_free_page(page->frame->kva);
    	return false; // 읽은 바이트 수가 예상한 바이트 수와 다르면 실패
	}


 	memset(page->frame->kva + bytes_read, 0, lazy_load_arg->page_zero_bytes);
	/* TODO: 이 함수는 VA 주소에서 첫 번째 페이지 폴트가 발생했을 때 호출됩니다. */
	/* TODO: 이 함수를 호출할 때 VA는 사용 가능합니다. */
	
	// 파일에서 페이지를 실제로 읽어와 메모리에 로드하는 로직을 구현해야 합니다.
	// 이때 aux에는 파일 오프셋, 읽기 바이트 수 등의 정보가 들어갈 수 있습니다.
	// 성공적으로 페이지를 로드하면 true를 반환하고, 실패하면 false를 반환해야 합니다.

	return true;
}


/* 파일에서 OFS 오프셋부터 시작해, 가상 주소 UPAGE에 세그먼트를 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES 만큼의 가상 메모리를 초기화합니다:
 *
 * - UPAGE에서 시작해 READ_BYTES 만큼의 데이터를 파일에서 읽습니다.
 *   파일에서 읽는 작업은 OFS 오프셋부터 시작합니다.
 *
 * - UPAGE + READ_BYTES 위치에서 시작해 ZERO_BYTES 만큼의 공간을 0으로 초기화합니다.
 *
 * 이 함수로 초기화된 페이지는 WRITABLE이 true이면 사용자 프로세스에서 쓰기가 가능하고,
 * 그렇지 않으면 읽기 전용입니다.
 *
 * 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를 반환하고,
 * 성공하면 true를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);  // 읽을 바이트와 0으로 채울 바이트의 합이 페이지 크기의 배수인지 확인
	ASSERT (pg_ofs (upage) == 0);  // upage가 페이지 크기 정렬되어 있는지 확인
	ASSERT (ofs % PGSIZE == 0);    // 파일 오프셋도 페이지 크기의 배수여야 함

	// 남은 데이터를 처리할 때까지 반복합니다.
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산합니다.
		 * 파일에서 PAGE_READ_BYTES 만큼 읽고
		 * 나머지 PAGE_ZERO_BYTES 만큼은 0으로 채웁니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;  // 현재 페이지에 읽을 바이트 크기 계산
		size_t page_zero_bytes = PGSIZE - page_read_bytes;  // 남은 바이트는 0으로 채움

		/* TODO: lazy_load_segment에 전달할 aux를 설정합니다. */
		struct lazy_load_arg *lazy_load_arg = (struct lazy_load_arg *)malloc(sizeof(struct lazy_load_arg));
		lazy_load_arg->file = file;
		lazy_load_arg->ofs = ofs;
		lazy_load_arg->page_read_bytes = page_read_bytes;
		lazy_load_arg->page_zero_bytes = page_zero_bytes;


		/* 페이지를 초기화하고, 페이지 할당이 실패하면 false를 반환합니다. */
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, lazy_load_arg))
			return false;  // 페이지 할당 실패 시 false 반환

		/* 다음 페이지로 이동. */
		read_bytes -= page_read_bytes;  // 읽은 바이트 수만큼 남은 읽기 바이트에서 차감
		zero_bytes -= page_zero_bytes;  // 0으로 채운 바이트 수만큼 남은 zero_bytes에서 차감
		ofs += page_read_bytes;
		upage += PGSIZE;  // 다음 페이지로 포인터를 이동
	}

	return true;  // 모든 페이지가 성공적으로 로드되면 true 반환
}


/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	//USER_STACK에서 페이지 크기(PGSIZE)를 뺀 값을 stack_bottom에 저장하여 스택의 하단 주소를 계산
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

    /* TODO: stack_bottom에 스택을 매핑하고 페이지를 즉시 할당합니다.
     * TODO: 성공하면 rsp를 설정합니다.
     * TODO: 페이지를 스택으로 표시해야 합니다. */
    /* TODO: 여기에 코드를 작성하세요 */

	// vm_alloc_page 함수는 stack_bottom에 페이지를 할당합니다.
	// VM_ANON과 VM_MARKER_0 플래그를 사용하여 익명 페이지를 생성합니다.
	// VM_MARKER_0: 페이지가 스택을 위한 것임을 나타내는 마커.
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, 1)){
		// 페이지 할당이 성공하면 해당 페이지를 프로세스에 할당합니다.
		success = vm_claim_page(stack_bottom);
        if(success){
			// 프로세스에 페이지가 성공적으로 할당되면 rsp를 USER_STACK로 설정합니다.
			// 스택이 정상적으로 초기화되었음. 새로운 스택 만들어짐!
            if_->rsp = USER_STACK;
        }
    }

	return success;
}
#endif /* VM */
