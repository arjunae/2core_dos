#include <stdint.h>
#include "include/aputils.h"      // relative heap allocator (rheapInit / rmalloc)
#include "core_shared.h"  // TaskInt, QUEUE_RESULT and other shared definitions
 
// Set to 0 for production builds to compile out all VGA debug output.
#define WORKER_DEBUG 1
 
#if WORKER_DEBUG
	// Status display in the top-right of VGA row 0.
	//
	// One slot per "class" of status. Letters that belong to the same class
	// share a column and overwrite each other, so each slot always shows that
	// class's current state. Letters of different classes live in different
	// columns and therefore never clobber one another - so on a crash/hang you
	// can read off, per class, the last state that was reached.
	//
	//   col 76  lifecycle  : I = init/setup      -> R = running (loop entered)
	//   col 77  loop state  : P = processing job <-> W = waiting (idle)
	//   col 78  allocation  : A = allocating      -> E = alloc failed
	//   col 79  result      : Q = posting result
	#define VGA_STATUS_BASE 76u
	#define VGA_STATUS_COL(c) ( \
		((c) == 'I' || (c) == 'R') ? 0u : \
		((c) == 'P' || (c) == 'W') ? 1u : \
		((c) == 'A' || (c) == 'E') ? 2u : \
		((c) == 'Q')               ? 3u : 3u)
	// Write 'ch' into its class column, but only when it actually changes the
	// character shown there: a redundant store of the same letter (e.g. 'W' on
	// every spin) is skipped, while a different letter of the *same* class
	// overwrites it (e.g. I -> R). Different classes never share a column.
	#define PRINT_VGA(ch) do { \
		uint8_t  vga_ch   = (uint8_t)(ch); \
		uint32_t vga_addr = 0xB8000u + (VGA_STATUS_BASE + VGA_STATUS_COL(vga_ch)) * 2; \
		uint8_t  cur_ch; \
		asm volatile("movb %%fs:(%1), %0" : "=r"(cur_ch) : "r"(vga_addr)); \
		if (vga_ch != cur_ch) { \
			uint16_t vga_val = ((uint16_t)0x1F << 8) | vga_ch; \
			asm volatile("movw %0, %%fs:(%1)" :: "r"(vga_val), "r"(vga_addr) : "memory"); \
		} \
	} while (0)
#else
	#define PRINT_VGA(ch) ((void)0)
#endif
 
// Cache flush helpers.
//
// On a strongly-ordered x86 core with write-back memory and a single
// producer/consumer, a compiler barrier is enough to keep the generated code
// from reordering stores around these points. They are written as macros so
// that on a weaker platform (or with non-coherent memory) you can drop in a
// real cache-line flush / hardware fence here without touching the call sites.
#define PUB_FLUSH(p, n) asm volatile("" ::: "memory")  // "result is now visible"
#define PUB_FENCE()     asm volatile("" ::: "memory")  // "ordering checkpoint"

 
// Pointer to the task interface, kept globally so it stays reachable for the
// lifetime of the worker (handy when extending this example, e.g. fault
// handlers that need to report back through the task).
volatile void* g_tskPtr = nullptr;
 
static void setMemWbAP(uint32_t linAddr, uint32_t sizeVal) {
	uint32_t cr3Val;
	asm volatile("mov %%cr3, %0" : "=r"(cr3Val));
	uint32_t dirPhys = cr3Val & 0xFFFFF000u;
	uint32_t endAddr = linAddr + sizeVal;
	
	while (linAddr < endAddr) {
		uint32_t dirIdx = linAddr >> 22;
		uint32_t dirAddr = dirPhys + dirIdx * 4;
		uint32_t dirVal;
		asm volatile("movl %%fs:(%1), %0" : "=r"(dirVal) : "r"(dirAddr));
		
		if (!(dirVal & 1)) {
			linAddr += 4096;
			continue;
		}
		
		if (dirVal & 0x00000080u) {
			dirVal &= ~0x00000018u;
			asm volatile("movl %0, %%fs:(%1)" :: "r"(dirVal), "r"(dirAddr) : "memory");
			linAddr = (linAddr + 0x00400000u) & 0xFFC00000u;
			if (linAddr == 0) break;
			continue;
		}
		
		uint32_t tblPhys = dirVal & 0xFFFFF000u;
		uint32_t tblIdx = (linAddr >> 12) & 0x03FFu;
		uint32_t tblAddr = tblPhys + tblIdx * 4;
		uint32_t tblVal;
		asm volatile("movl %%fs:(%1), %0" : "=r"(tblVal) : "r"(tblAddr));
		
		tblVal &= ~0x00000018u;
		asm volatile("movl %0, %%fs:(%1)" :: "r"(tblVal), "r"(tblAddr) : "memory");
		
		linAddr += 4096;
	}
	asm volatile("mov %0, %%cr3" :: "r"(cr3Val) : "memory");
}


//  This is the function the host jumps to. It is forced into its own section
//  (.text.entry) so the loader can place it at a known offset, and
//  force_align_arg_pointer guarantees a correctly aligned stack on entry even
//  if the host's call site didn't align it.
//
//  Parameters (all PHYSICAL addresses):
//    dataPhys : physical address of the struct share_mem control block
//    taskAddr : physical address of the TaskInt task-interface struct
//    workPhys : physical load address of this worker blob; the origin for the
//               DS-relative addressing scheme (see header notes).
//
//  This function never returns: a worker runs until the core is reset/halted.
extern "C" __attribute__((section(".text.entry"), force_align_arg_pointer))
void usrWork(uint32_t dataPhys, uint32_t taskAddr, uint32_t workPhys) {
	// 'I' on the lifecycle column: the worker has started executing.
	PRINT_VGA('I');
 
	// Convert the incoming physical addresses to DS-relative C pointers.
	uint32_t rel_tsk_adr = taskAddr - workPhys;
	uint32_t rel_dat_adr = dataPhys - workPhys;
	TaskInt* tsk_ptr = (TaskInt*)rel_tsk_adr;
 
	// Keep the task pointer reachable for the worker's whole lifetime.
	g_tskPtr = (volatile void*)tsk_ptr;
 
	// Make the shared control block and the task struct Write-Back cacheable.
	//setMemWbAP(dataPhys, sizeof(struct share_mem));
	//setMemWbAP(taskAddr, sizeof(TaskInt));
 
	// Tell the host we're alive: status = 2 (running). The exact status codes
	// are defined by the library; 2 is the "worker is up" handshake value.
	tsk_ptr->status = ApStatus::Ready;
	PUB_FLUSH(&tsk_ptr->status, 1);  // make the status visible to the host
	PUB_FENCE();
 
	// Map the shared control block and grab pointers to its hot fields.
	volatile struct share_mem* share_dat = (volatile struct share_mem*)(rel_dat_adr);
	volatile uint32_t* head_ptr = &share_dat->head_idx;
	volatile uint32_t* tail_ptr = &share_dat->tail_idx;
	volatile uint32_t* max_ptr  = &share_dat->max_item;
 
	// Set up the shared result heap. It arrives as a physical address; make it
	// Write-Back, convert it to a DS-relative pointer, then hand it to the
	// relative-address allocator.
	uint32_t heap_val = share_dat->heap_ptr;
	uint32_t heap_sze = share_dat->heap_sze;
 
	//setMemWbAP(heap_val, heap_sze);
	heap_val -= workPhys;                       // physical -> DS-relative
	rheapInit((void*)heap_val, heap_sze);
 
	// Cache our local copy of the consumer index.
	uint32_t head_idx = *head_ptr;
 
	// 'R' on the lifecycle column: setup complete, entering the service loop.
	PRINT_VGA('R');
 
	// ------------------------------------------------------------------------
	//  Main service loop: drain the ring buffer forever.
	// ------------------------------------------------------------------------
	while (1) {
		uint32_t tail_idx = *tail_ptr;  // re-read the producer index each pass
 
		if (head_idx != tail_idx) {
			// ---- A job is waiting --------------------------------------------
			// 'P': we are processing a job.
			PRINT_VGA('P');
 
			volatile struct work_slt* slt_cur = &share_dat->slots[head_idx];
			uint32_t val_in = slt_cur->val_in;
 
			// >>> Replace this with your real computation. <<<
			// Dummy work for the example: square the input and scale by 42.
			uint32_t calc_res = val_in * val_in * 42;
 
			// 'A': allocating a buffer to hold the result.
			PRINT_VGA('A');
			uint32_t* ptr_res = (uint32_t*)rmalloc(sizeof(uint32_t));
 
			if (ptr_res) {
				// Store the result and publish it before notifying the host.
				*ptr_res = calc_res;
				PUB_FLUSH(ptr_res, sizeof(uint32_t));
				PUB_FENCE();
 
				// 'Q': posting the result to the host's result queue.
				PRINT_VGA('Q');
				// Hand the result back as a PHYSICAL address (+ workPhys) so the
				// host can find it. Arguments are defined by the library:
				//   tsk_ptr                 -> task interface to post through
				//   0xABCD                  -> result tag / magic identifier
				//   1                       -> result type / count
				//   (ptr)+workPhys          -> physical address of the payload
				//   1                       -> flags
				QUEUE_RESULT(tsk_ptr, 0xABCD, 1, (uint32_t)ptr_res + workPhys, 1);
			} else {
				// 'E': allocation failed; the result is dropped this iteration.
				PRINT_VGA('E');
			}
 
			// Mark the slot consumed so the host can recycle it.
			slt_cur->is_done = 1;
 
			// Advance the consumer index (wrap around the ring).
			head_idx = (head_idx + 1) % (*max_ptr);
			*head_ptr = head_idx;
 
			// Publish the new head so the host sees the slot as free.
			PUB_FLUSH((void*)head_ptr, 4);
			PUB_FENCE();
		} else {
			// ---- Queue empty -------------------------------------------------
			// 'W': idle, waiting for the host to enqueue more work.
			PRINT_VGA('W');
			// PAUSE hints the CPU that this is a spin-wait: it reduces power and
			// avoids hammering the memory bus while polling.
			asm volatile("pause" ::: "memory");
		}
	}
}
