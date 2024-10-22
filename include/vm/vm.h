#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "userprog/process.h"


enum vm_type { 
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1, 		
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#include "lib/kernel/hash.h"

#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "페이지"의 표현입니다.
 * 이 구조체는 "부모 클래스"이며, 네 가지 "자식 클래스"를 가지고 있습니다.
 * 이 자식 클래스들은 uninit_page, file_page, anon_page, 그리고 페이지 캐시 (project4)입니다.
 * 이 구조체의 미리 정의된 멤버는 삭제하거나 수정하지 마세요. */
struct page {
	const struct page_operations *operations;  /* 페이지에 대해 수행할 수 있는 동작들을 정의한 함수 포인터들의 집합 */
	void *va;              /* 사용자 공간에서의 가상 주소 -> 프로세스의 주소공간에서 각 페이지가 어느 위치에 매핑되었는지 나타냄. */
	struct frame *frame;   /* 이 페이지와 연결된 물리적 frame을 가리키는 포인터, 얘도 대개 4KB, 
	가상 메모리의 페이지는 프레임에 매핑되며, 실제 메모리 접근은 이 프레임을 통해 이루어집니다 */
	
	/* 여러분의 구현이 들어갈 부분입니다 */
	
	bool writable; // write 가능, 불가능
	bool is_loaded; // 물리메모리의 탑재 여부를 알려주는 플래그
	struct hash_elem elem;    	// 해시 테이블 elem
	
	struct list_elem mmap_elem; // mmap 리스트 elem
	size_t swap_slot;	// 스왑 슬롯
	int mapped_page_count;

	/* 유니언(Union) 필드는 C 언어에서 사용하는 데이터 구조로, 하나의 메모리 공간을 여러 멤버가 공유하는 방식입니다. 
	 즉, 유니언에 포함된 여러 멤버 중 하나의 멤버만 특정 시점에 값을 가질 수 있으며, 유니언 필드의 크기는 가장 큰 멤버의 크기로 결정됩니다.
	 페이지의 유형별 데이터를 저장하기 위해 사용되는 유니언. 한 번에 하나의 멤버만 값을 가질 수 있는 자료형임. 
	 * 해당 페이지가 어떤 종류인지는 operations 필드와 함께 이 유니언으로 관리된다. 
	  union은 크기가 가장 큰 멤버의 크기만큼을 할당받아 멤버들이 그 메모리를 공유함. 
	  페이지가 익명 페이지라면 struct page 안에서 anon_page가 활성화되고, 익명 페이지에 필요한 모든 정보를 여기에 저장한다.*/ 
	
	union {	// 유니언 필드 -> 한 메모리 영역에 여러 데이터 타입을 저장할 수 있는 특별한 데이터 타입. 하지만 동시에 하나의 데이터만 저장할 수 있음.
		struct uninit_page uninit;		/* 페이지가 아직 명시적으로 초기화 되지 않은 페이지 상태 -> 페이지 폴트나 실제로 메모리 필요시 이 페이지가 초기화 됨. */
		struct anon_page anon;			/* 특정 파일에 매핑되지 않은 메모리 블록 , 스왑 영역으로 부터 데이터를 불러오거나 스왑 영역에 저장 */
		struct file_page file;			/* 파일에 매핑된 페이지 정보를 저장 , 파일의 특정 부분을 메모리로 매핑하여 직접 접근할 수 있게 하는 기능 */
	#ifdef EFILESYS
		struct page_cache page_cache;   /*  파일 시스템에서 데이터를 자주 읽거나 쓰는 페이지를 메모리에 캐시해두고, 디스크 접근을 최소화하기 위해 사용 */
	#endif
  };
};

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);	/* 페이지를 스왑 인 하는 함수 포인터 */
	bool (*swap_out) (struct page *);			/* 페이지를 스왑 아웃하는 함수 포인터 */
	void (*destroy) (struct page *);			/* 페이지를 파괴하는 함수 포인터 */	
	enum vm_type type;							/* 페이지의 타입을 나타내는 열거형 */
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)


// struct spt_entry {

// 	uint8_t type;	// vm_bin, file, anon의 타입
//     void *vaddr;   // spt_entry가 관리하는 가상페이지 번호
// 	bool writable; // write 가능, 불가능

// 	bool is_loaded; // 물리메모리의 탑재 여부를 알려주는 플래그
// 	struct file *file; // 가상주소와 맵핑된 파일

// 	struct list_elem mmap_elem; // mmap 리스트 elem

// 	size_t offset; // 읽어야 할 파일 오프셋
// 	size_t read_bytes; 	// 가상페이지에 쓰여져 있는 데이터 크기
// 	size_t zero_bytes;	// 0으로 채울 남은 페이지의 바이트

// 	size_t swap_slot;	// 스왑 슬롯
    
// 	struct hash_elem elem;    	// 해시 테이블 elem
// 	struct page *page;

// };
/* 현재 프로세스의 메모리 공간을 표현한 구조체입니다.
 * 이 구조체에 대해 특정한 설계를 강요하지 않습니다.
 * 모든 설계는 여러분에게 달려 있습니다. */
struct supplemental_page_table {
	struct hash spt_hash;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
static unsigned spt_hash_func (const struct hash_elem *e, void *aux UNUSED);
static bool spt_less_func (const struct hash_elem *a, const struct hash_elem *b);

bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);
void hash_page_destroy(struct hash_elem *e, void *aux);

#endif  /* VM_VM_H */
