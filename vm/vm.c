/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/palloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"


// #define MAX_STACK_SIZE (1024 * 1024) // 1MB

// bool 
// is_stack_access(uintptr_t rsp, void *addr) {
//     // 스택의 하단 주소 계산
//     void *stack_bottom = (void *)((uintptr_t)USER_STACK - MAX_STACK_SIZE);
    
//     // 주소가 스택의 하단부터 스택의 최상단까지 범위 내에 있는지 확인
//     return (addr >= stack_bottom && addr < USER_STACK);
// }
/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* 초기화 함수를 사용하여 보류 중인 페이지 객체를 생성합니다. 
 * 페이지를 직접 생성하지 말고, 이 함수나 `vm_alloc_page`를 통해 생성하세요. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* upage가 이미 점유되었는지 여부를 확인합니다. */
	if (spt_find_page (spt, upage) == NULL) {
		
		//* TODO: 페이지를 생성하고, VM 유형에 맞는 초기화 함수를 가져온 후,
		struct page *page = (struct page*)malloc(sizeof(struct page));
		if (page == NULL) 
			return false;
		
		// 이 선언은 함수 포인터를 의미합니다. 함수 포인터는 특정한 함수의 주소를 저장하고, 
		// 그 함수를 나중에 호출할 수 있게 하는 변수입니다.
		// initializer는 나중에 호출될 때 page 구조체와 type, 그리고 추가 데이터(void *)를 받아 bool 값을 반환하는 함수
		bool (*initializer)(struct page *, enum vm_type, void *);
		switch (VM_TYPE(type))
		{
		case VM_ANON: //Uninit은 uninit_new에 있음.
			initializer = anon_initializer;
			break;
		case VM_FILE:
			initializer = file_backed_initializer;
			break;
		}
		//* TODO: uninit_new 함수를 호출하여 "uninit" 페이지 구조체를 생성합니다.
		uninit_new(page, upage, init, type, aux, initializer);
		//* TODO: uninit_new를 호출한 후 해당 필드를 수정해야 합니다.
		page->writable = writable;

		//* TODO: 페이지를 spt에 삽입합니다.
		return spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page * 
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL; // 찾은 페이지를 저장할 변수, 처음엔 NULL로 초기화
	struct hash *spt_hash = &spt->spt_hash;

	// 가상 주소는 페이지 단위로 관리되므로 va를 페이지 경계로 정렬해야함.
	int aligned_va = pg_round_down(va);

	// // page 구조체에 대한 임시 객체 생성 (메모리 할당은 필요 없음)
	struct page temp_page; // 얘는 임시로 사용할 애라 메모리 할당이 없어서 포인터를 사용하면 안된다. 
	temp_page.va = aligned_va; // 검색할 키로 사용될 가상주소를 할당
	
	struct hash_elem *find_elem = hash_find(spt_hash, &temp_page.elem); // 주어진 해시 요소를 검색함. 해시 요소를 이용해 검색을 시도함. 검색 결과가 있으면 find_elem에 해당 요소의 주소가 저장됨.

	if (find_elem != NULL) {
		page = hash_entry(find_elem, struct page, elem); // 검색된 해시 요소를 페이지 구조체로 변환
		
	}

	// 검색된 페이지를 반환하거나, 없으면 NULL 반환
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	bool succ = false; // 함수의 초기 상태 false로 설정

	struct hash *spt_hash = &spt->spt_hash;  

	if (spt_find_page(spt, page->va) == NULL) { // 페이지의 가상 주소에 대한 중복 체크
    	
		// hash_insert: 해시에 페이지를 삽입 (hash_elem을 통해 삽입)
		// 동일한 요소가 이미 존재하면 기존 요소를 반환하고, 없다면 NULL을 반환
		struct hash_elem *result = hash_insert(spt_hash, &page->elem); // 해시 테이블에 새로운 spt_entry를 삽입함. hash_insert함수는 삽입 결과를 반환함.
		
		if (result == NULL) // 삽입이 성공하면 result는 NULL이 됨. 
			succ = true;
	}	

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}


/* palloc()을 호출하여 프레임을 가져옵니다. 사용 가능한 페이지가 없으면,
페이지를 교체(evict)하고 그것을 반환합니다. 이 함수는 항상 유효한 주소를 반환합니다.
즉, 사용자 풀 메모리가 가득 차 있으면, 이 함수는 프레임을 교체하여
사용할 수 있는 메모리 공간을 확보합니다.*/
static struct frame *
vm_get_frame (void) {
	// 사용자가 요청한 페이지를 할당하고, 그 페이지의 커널 가상주소(KVA)를 할당받음
	// 이 주소를 통해 커널에서 페이지를 관리할 수 있게 됨.
	void *get_kva = palloc_get_page(PAL_USER);  

	if (get_kva == NULL) //실패시 패닉
		PANIC("todo");

	struct frame *frame = malloc(sizeof(struct frame)); // 프레임 구조체를 위한 메모리 할당
	frame->kva = get_kva; // 커널 가상 주소를 프레임에 설정 -> 얘를통에 이 페이지에 접근할 수 있음.
	frame->page = NULL; // 프레임의 페이지를 초기화, 현재 어떤 페이지와도 연결되지 않음.

	ASSERT (frame != NULL); // 프레임이 NULL이 아닌지 확인
	ASSERT (frame->page == NULL); // 프레임의 페이지가 NULL인지 확인
	return frame; // 초기화된 프레임 반환
}

/* Growing the stack. */
bool
vm_stack_growth (void *addr UNUSED) {
	
	uintptr_t aligned = pg_round_down(addr);
	for (; aligned < USER_STACK - PGSIZE; aligned += PGSIZE) {
		if (!spt_find_page(&thread_current()->spt, aligned))
			vm_alloc_page(VM_ANON, aligned, true);
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success 페이지 폴트를 처리하는 데 사용되는 함수*/
// addr : 페이지 폴트가 발생한 주소, 이 주소를 기반으로 페이지를 찾거나 할당.
// 나머지 플래그 : 폴트가 사용자 모드에서 발생했는지, 쓰기 작업인지, 페이지가 실제로 존재하지 않는지 등의 정보
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	// 스택 확장 여부를 판단
    // if (is_stack_access(f->rsp, addr)) {
    //     if (!vm_stack_growth(addr)) { // 반환값 확인
    //         return false; // 스택 확장이 실패한 경우
    //     }
    // }
	// 주소가 NULL이거나 커널 주소이거나 not_present가 false인 경우
	
	// 페이지 테이블에서 주소에 해당하는 페이지 검색
	page = spt_find_page(spt, addr);  

	if (page == NULL) {
		if (addr >= USER_STACK_LIMIT) {
			uintptr_t diff = f->rsp - (uintptr_t)addr;
			if (diff <= 8){
				vm_stack_growth(addr);
				page = spt_find_page(spt, addr); 
				if(page == NULL) // 늘려줬는데도 page가 NULL이면 false
					return false;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}

	// write 불가능한 페이지에 write 요청한 경우
	if (write == 1 && page->writable == 0) 
		return false;

	return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);  /*컴파일러가 페이지 타입에 따라 적절한 파괴 함수를 호출*/
	free (page);
}

/* 가상 메모리 시스템에서 특정 가상 주소에 대해 물리 메모리 페이지를 확보(클레임)하는 함수 */
bool
vm_claim_page (void *va UNUSED) {
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = spt_find_page(spt, va);  // 주어진 가상 주소에 해당하는 페이지를 찾음

	/* TODO: Fill this function */
	if (page == NULL)
		return false;

	return vm_do_claim_page (page); // 페이지가 이미 존재하고 할당된 상태라면 추가적인 작업 없이 바로 리턴됨. 존재하면 함수를 호출하여 페이지 확보
}

/* 페이지 클레임의 실제 작업을 수행하는 내부 함수,  실제로 페이지를 프레임에 연결하고, 페이지 테이블을 설정하는 등의 구체적인 작업을 수행 */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame (); // 새로운 프레임 할당. 이때 프레임이란 실제 물리 메모리의 페이지이며 페이지를 매핑할 대상임.
	if (frame == NULL)  
		return false;

	/* 프레임과 페이지 양방향 연결 */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* **페이지의 가상 주소(va)**를 **프레임의 커널 가상 주소(kva)**에 매핑하기 위해 pml4_set_page()를 호출합니다.
	pml4_set_page()는 현재 스레드의 PML4(Page Map Level 4) 페이지 테이블에서 가상 주소 page->va를 프레임의 물리 주소에 매핑하는 역할을 합니다.
	페이지가 쓰기 가능한 경우 page->writable 플래그에 따라 읽기/쓰기를 설정합니다.
	매핑에 실패하면 false를 반환합니다.*/
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) {
		// vm_evict_frame();
		return false;
	}

	return swap_in (page, frame->kva); // 페이지가 스왑 아웃되어 있었다면, 해당 페이지를 다시 메모리로 읽어들이는 작업을 수행 
	// 스왑 작업은 커널 영역에서 이루어짐. 따라서 커널이 직접 물리 메모리에서 데이터를 관리하거나 이동할 때는 KVA를 사용
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {

	struct hash *spt_hash = &spt->spt_hash;
	hash_init(spt_hash, spt_hash_func, spt_less_func, NULL);
}

static unsigned spt_hash_func (const struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry(e, struct page, elem);
	return hash_bytes(&page->va, sizeof(page->va)); // 64비트에서는 hash_int를 쓰는것보단 hash_bytes를 쓰는게 안전함.
}

static bool spt_less_func (const struct hash_elem *a, const struct hash_elem *b) {
	struct page *page_a = hash_entry(a, struct page, elem);
	struct page *page_b = hash_entry(b, struct page, elem);

	return page_a->va < page_b->va; // 가상 주소를 기준으로 비교
}

/* Copy supplemental page table from src to dst */
void page_copy(struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry(e, struct page, hash_elem);
	int type = VM_TYPE(page->operations->type);

	void *aux_copy = NULL;

	if (type == VM_UNINIT) {
		if (page->uninit.aux) {
			aux_copy = malloc(sizeof(struct file_page));
			memcpy(aux_copy, page->uninit.aux, sizeof(struct file_page));
		}
		vm_alloc_page_with_initializer(page->uninit.type, page->va, page->writable, page->uninit.init, aux_copy);
	}
	else if (type == VM_ANON) {
		// 이미 초기화된 ANON 타입의 페이지
		vm_alloc_page(page->operations->type, page->va, page->writable);

		// 만약 프레임이 있으면, 해당 프레임을 자식 프로세스의 페이지로 복사
		if (page->frame != NULL) {
			vm_claim_page(page->va);
			struct page *page_child = spt_find_page(&thread_current()->spt, page->va);
			memcpy(page_child->frame->kva, page->frame->kva, PGSIZE);
		}
	}
}
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	hash_apply(&src->page_hash, page_copy);
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_clear(&spt->spt_hash, hash_page_destroy); // 해시 테이블의 모든 요소를 제거
}

void 
hash_page_destroy(struct hash_elem *e, void *aux) {

    struct page *page = hash_entry(e, struct page, elem);
    vm_dealloc_page(page);

}
