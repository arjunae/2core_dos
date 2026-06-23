#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <signal.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/nearptr.h>

#include "../2core.h"
#include "core_shared.h"

static __dpmi_meminfo         g_heap_map;        // our heap phys mapping
static volatile sig_atomic_t  g_heap_mapped = 0; // is it currently mapped?

// Release the heap mapping. safe from atexit and a signal handler.
static void free_heap_mapping(void) {
	if (g_heap_mapped) {
		g_heap_mapped = 0;
		__dpmi_free_physical_address_mapping(&g_heap_map);
	}
}

// SIGINT/SIGTERM path: the library's catchSig runs FIRST (it resets the signals
// to SIG_DFL and stops the AP via finish()), then chains here. catchSig does not
// terminate when a previous handler exists, so we free our mapping and exit like
// it otherwise would. exit() then also runs the library's atexit(endTask).
static void on_signal(int sig) {
	(void)sig;
	free_heap_mapping();
	exit(1);
}

// The library doesn't trap these. on Vault, stop AP and release our mapping, then re-raise so the status reflects it.
static void on_crash(int sig) {
	if (globTask != NULL) {
		core2.finish();
	}
	free_heap_mapping();
	signal(sig, SIG_DFL);
	raise(sig);
}

// Responsible for allocating shared resources and dispatching work to the Application Processor
int main(void) {
	if (!__djgpp_nearptr_enable()) {
		return 1;
	}
	
	// Register our heap-mapping teardown for the paths the library does not cover.
	// SIGINT/SIGTERM are installed before setupSys so the library saves on_signal as oldInt/oldTerm 
	// and chains to it from catchSig (after it has already stopped the AP). 
	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);
	atexit(free_heap_mapping);
	
	if (!core2.setupSys("mock_wk.bin")) {
		return 1;
	}
	
	// The library does not trap faults; add our own so a BSP crash still stops the AP and frees the mapping. 
	signal(SIGABRT, on_crash);
	signal(SIGFPE,  on_crash);
	signal(SIGSEGV, on_crash);
	
	uint32_t data_off = core2.getDataOff();
	uint32_t task_off = core2.taskOff;
	
	// Initialize the shared memory ring buffer control variables to zero
	// The maximum item count defines the ring depth
	_farpokel(_dos_ds, data_off + offsetof(struct share_mem, head_idx), 0);
	_farpokel(_dos_ds, data_off + offsetof(struct share_mem, tail_idx), 0);
	_farpokel(_dos_ds, data_off + offsetof(struct share_mem, max_item), 32);
	
	// Define the high memory area for dynamic allocations made by the Application Processor
	// We pass the physical address and total size via the shared memory structure
	// needs hdpmi -x
	uint32_t heapPhys = 256 * 1024 * 1024;
	uint32_t heapSize = 16 * 1024 * 1024;
	
	_farpokel(_dos_ds, data_off + offsetof(struct share_mem, heap_ptr), heapPhys);
	_farpokel(_dos_ds, data_off + offsetof(struct share_mem, heap_sze), heapSize);
	
	g_heap_map.address = heapPhys;
	g_heap_map.size = heapSize;
	if (__dpmi_physical_address_mapping(&g_heap_map) != 0) {
		return 1; // atexit/endTask
	}
	g_heap_mapped = 1;
	
	// Establish a linear pointer to the newly mapped high memory area
	// This allows the BSProcessor to read results written by the Application Processor
	uint8_t* heapLin = (uint8_t*)(g_heap_map.address + __djgpp_conventional_base);
	
	// Clear all completion flags in the job slots to prevent processing stale data
	for (int i = 0; i < 32; i++) {
		_farpokel(_dos_ds, data_off + offsetof(struct share_mem, slots[0].is_done) + i * sizeof(struct work_slt), 0);
	}
	
	// Prepare the system result ring buffer which signals completed dynamic memory tasks
	_farpokel(_dos_ds, task_off + offsetof(struct TaskInt, resRing.headIdx), 0);
	_farpokel(_dos_ds, task_off + offsetof(struct TaskInt, resRing.tailIdx), 0);
	_farpokel(_dos_ds, task_off + offsetof(struct TaskInt, resRing.maxItem), 32);
	
	// Wake up the Application Processor and provide the physical address of the worker binary
	core2.startAp(1);
	core2.spawn(core2.workerPhys);
	
	int sent_job = 0;
	int recv_job = 0;
	
	// Seed the random number generator using the current system time
	// This guarantees a different sequence of calculation inputs on every execution
	srand(time(NULL));
	
	// Local array to store sent values for later output formatting
	// This helps trace back the original input when printing the final calculated result
	uint32_t sent_vals[10];
	
	// Producer loop pushing random numbers into the shared ring buffer
	while (sent_job < 10) {
		uint32_t max_item = _farpeekl(_dos_ds, data_off + offsetof(struct share_mem, max_item));
		uint32_t tail_idx = _farpeekl(_dos_ds, data_off + offsetof(struct share_mem, tail_idx));
		uint32_t nxt_tail = (tail_idx + 1) % max_item;
		
		// Spinlock waiting for the Application Processor to consume items if the ring is full
		while (nxt_tail == _farpeekl(_dos_ds, data_off + offsetof(struct share_mem, head_idx))) {
			asm volatile("pause" ::: "memory");
		}
		
		uint32_t slot_off = data_off + offsetof(struct share_mem, slots[0]) + tail_idx * sizeof(struct work_slt);
		uint32_t rand_val = rand() % 100;
		
		sent_vals[sent_job] = rand_val;
		
		// Write the new job data into the calculated slot offset
		_farpokel(_dos_ds, slot_off + offsetof(struct work_slt, val_in), rand_val);
		_farpokel(_dos_ds, slot_off + offsetof(struct work_slt, is_done), 0);
		
		// Compiler memory barrier ensuring all data writes are committed before updating the tail pointer
		asm volatile("" ::: "memory");
		_farpokel(_dos_ds, data_off + offsetof(struct share_mem, tail_idx), nxt_tail);
		
		printf("Sent job %d with input %u\n", sent_job, rand_val);
		sent_job++;
	}
	
	// Consumer loop polling the system result ring for completed tasks
	while (recv_job < 10) {
		uint32_t head_idx = _farpeekl(_dos_ds, task_off + offsetof(struct TaskInt, resRing.headIdx));
		uint32_t tail_idx = _farpeekl(_dos_ds, task_off + offsetof(struct TaskInt, resRing.tailIdx));
		uint32_t max_item = _farpeekl(_dos_ds, task_off + offsetof(struct TaskInt, resRing.maxItem));
		
		if (tail_idx != head_idx) {
			uint32_t slot_bse = task_off + offsetof(struct TaskInt, resRing.dataArr[0]) + tail_idx * sizeof(struct ResultSlot);
			uint32_t tckt_id = _farpeekl(_dos_ds, slot_bse + offsetof(struct ResultSlot, ticketId));
			uint32_t rdy_flag = _farpeekl(_dos_ds, slot_bse + offsetof(struct ResultSlot, readyFlag));
			uint32_t res_phys = _farpeekl(_dos_ds, slot_bse + offsetof(struct ResultSlot, msgVal));
			
			// Verify the magic ticket identifier and check if the result is fully prepared
			if (tckt_id == 0xABCD && rdy_flag) {
				// Convert the returned physical memory address into our mapped linear space
				uint32_t res_off = res_phys - heapPhys;
				uint32_t* res_ptr = (uint32_t*)(heapLin + res_off);
				
				uint32_t orig_val = sent_vals[recv_job];
				printf("Recv job %d res %u (Formula %u * %u * 42) from addr %x\n", recv_job, *res_ptr, orig_val, orig_val, res_phys);
				recv_job++;
				
				// Acknowledge the receipt by clearing the ready flag in the system ring
				_farpokel(_dos_ds, slot_bse + offsetof(struct ResultSlot, readyFlag), 0);
			}
			
			// Advance the tail index to free up the system result slot
			tail_idx = (tail_idx + 1) % max_item;
			_farpokel(_dos_ds, task_off + offsetof(struct TaskInt, resRing.tailIdx), tail_idx);
			asm volatile("" ::: "memory");
		} else {
			asm volatile("pause" ::: "memory");
		}
	}
	
	// Normal exit: gracefully stop the AP while it is still alive (as before),
	// then release our heap mapping. The library's atexit(endTask) runs after
	// us as an INIT-based backstop.
	core2.finish();
	free_heap_mapping();
	return 0;
}
