#include <stdint.h>
#include <string.h>

#include "aputils.h" // Include 2CoreDOS Task definitions

#include "../../edge264-master/edge264.h"
#include "core_shared.h"

// =====================================================================
// STRIPPED BUILD
// ---------------------------------------------------------------------
// Alle hardware-nahen Bloecke, die in edge_cpu2_cc.1 NEU dazukamen und
// einzeln einen Crash ausloesen koennen, sind hier ENTFERNT:
//   - setMemWbAP()      (Page-Table-Edit + cr3-Reload)
//   - lfbSetWC()/MTRR   (wrmsr -> #GP-Risiko)
//   - apicTimerInit()   (unmaskierter Timer-IRQ Vektor 0x40 ohne Handler)
//   - rdmsr/wrmsr, mtrrTypeFor, phys_addr_bits, apicRead/apicWrite
//
// Damit verhaelt sich die Datei zur Laufzeit wie die alte, funktionierende
// edge264_cpu2.cc: Handshake -> Heap -> Decoder -> Loop.
//
// WICHTIG: Da die WB-Remap-/MTRR-Bloecke fehlen, darf NICHT auf MESI-
// Kohaerenz vertraut werden -> SHARED_MODE 0 (clflush + mfence), wie im
// funktionierenden Original. Erst wenn das wieder laeuft, einzeln die
// entfernten Bloecke wieder reinnehmen und SHARED_MODE testen.
// =====================================================================
#define SHARED_MODE 1

static inline void safe_clflush(volatile const void *p, size_t n) {
	const char *ptr = (const char *)p;
	const char *end = ptr + n;
	for (; ptr < end; ptr += 64)
		asm volatile("clflush (%0)" :: "r"(ptr) : "memory");
}

#if SHARED_MODE == 0
  #define PUB_FLUSH(p, n)       safe_clflush((p), (n))
  #define PUB_FENCE()           asm volatile("mfence" ::: "memory")
  #define PUB_FRAME_FENCE()     asm volatile("mfence" ::: "memory")
#else
  #define PUB_FLUSH(p, n)       ((void)0)
  #define PUB_FENCE()           asm volatile("" ::: "memory")
  #define PUB_FRAME_FENCE()     asm volatile("" ::: "memory")
#endif

volatile void* g_tskPtr = nullptr;

extern "C" __attribute__((section(".text.entry"), force_align_arg_pointer))
void cpu2_decoder_loop(uint32_t dataPhys, uint32_t taskAddr, uint32_t workerPhys) {
	uint32_t relTaskAddr = taskAddr - workerPhys;
	uint32_t relDataPhys = dataPhys - workerPhys;
	TaskInt* tskPtr = (TaskInt*)relTaskAddr;
	g_tskPtr = (volatile void*)tskPtr;

	// Progress-Marker ("XX" weiss auf rot) – zeigt, dass der AP ueberhaupt
	// in den C-Code gesprungen ist. Harmlos, bleibt drin.
	asm volatile("movl $0x2F582F58, %%fs:0xB8000" ::: "memory");

	// --- ENTFERNT: setMemWbAP(dataPhys/taskAddr) -----------------------
	// (Page-Table-Edit + cr3-Reload; Crash-Kandidat #3)

	uint32_t apicId;
	asm volatile("movl %%fs:0xFEE00020, %0" : "=r"(apicId) :: "memory");
	apicId = (apicId >> 24) & 0xFF;
	tskPtr->intEbx = apicId;
	PUB_FLUSH(&tskPtr->intEbx, 1);
	PUB_FENCE();
	tskPtr->status = ApStatus::Ready;                       // BSP-Handshake: ab hier "AP alive"
	PUB_FLUSH(&tskPtr->status, 1);
	PUB_FENCE();

	volatile EdgeData *sharedData = (volatile EdgeData *)(relDataPhys);
	volatile uint32_t *headIdxPtr = &sharedData->headIdx;
	volatile uint32_t *tailIdxPtr = &sharedData->tailIdx;
	volatile uint32_t *maxItemPtr = &sharedData->maxItem;

	uint32_t heapPtrVal = sharedData->heapPtr;
	uint32_t heapSzVal  = sharedData->heapSize;

	// --- ENTFERNT: setMemWbAP(heapPtrVal) ------------------------------

	heapPtrVal -= workerPhys;
	rheapInit((void*)heapPtrVal, heapSzVal);

	// --- ENTFERNT: lfbSetWC()/MTRR-Programmierung + MTRR-Diagnose-Dump --
	// (wrmsr -> #GP-Risiko; Crash-Kandidat #2)

	Edge264Decoder *dec = edge264_alloc(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr);
	if (!dec) return;

	// --- Frame-Puffer-Pool: ein Puffer pro Result-Ring-Slot ------------
	uint32_t poolN = tskPtr->resRing.maxItem;
	if (poolN > 64) poolN = 64;
	uint8_t* framePool[64]   = {nullptr};
	uint32_t framePoolSz[64] = {0};
	for (uint32_t i = 0; i < poolN; i++) { framePool[i] = nullptr; framePoolSz[i] = 0; }

	uint32_t headIdx = *headIdxPtr;

	// --- ENTFERNT: apicTimerInit() -------------------------------------
	// (unmaskierter periodischer Timer-IRQ Vektor 0x40; Crash-Kandidat #1)

	// Leichtgewichtige Transport-Diagnose (kostet nichts, hilft beim
	// "0 Frames?"-Debugging). Kann bleiben oder raus.
	uint32_t dbgNalTotal     = 0;
	uint32_t dbgFramesPushed = 0;

	while (1) {
		uint32_t tailIdx = *tailIdxPtr;

		if (headIdx != tailIdx) {
			volatile NalSlot *slot = &sharedData->slots[headIdx];
			volatile uint32_t *ptrPhys = &slot->ptr_phys;
			volatile uint32_t *sizePtr = &slot->size;
			volatile uint32_t *resPtr  = &slot->result;
			volatile uint32_t *donePtr = &slot->done;

			uint8_t *nal_buf = (uint8_t *)(*ptrPhys);
			nal_buf -= workerPhys;
			uint32_t nal_size = *sizePtr;

			int res = edge264_decode_NAL(dec, nal_buf, nal_buf + nal_size, nullptr, nullptr);

			Edge264Frame pic;
			while (edge264_get_frame(dec, &pic, 0) == 0) {
				uint32_t cH    = pic.height_Y / 2;
				uint32_t ySize = pic.stride_Y * pic.height_Y;
				uint32_t cSize = pic.stride_C * cH;
				uint32_t infoAligned = (sizeof(FrameInfo) + 31) & ~31;
				uint32_t need = infoAligned + ySize + 2 * cSize;

				uint32_t rMax     = tskPtr->resRing.maxItem;
				if (rMax == 0) rMax = 1;
				uint32_t curHead  = tskPtr->resRing.headIdx;
				uint32_t nextHead = (curHead + 1) % rMax;

				// Backpressure: warten bis der BSP diesen Slot konsumiert
				// hat, DANN seinen Puffer wiederverwenden.
				while (nextHead == tskPtr->resRing.tailIdx) {
					asm volatile("pause" ::: "memory");
				}

				// Puffer fuer diesen Slot bereitstellen (einmalig / bei Wachstum)
				if (framePoolSz[curHead] < need) {
					if (framePool[curHead]) rfree(framePool[curHead]);
					framePool[curHead]   = (uint8_t*)rmalloc(need);
					framePoolSz[curHead] = framePool[curHead] ? need : 0;
				}
				uint8_t *blk = framePool[curHead];
				if (!blk) continue;            // OOM -> Frame skippen

				FrameInfo *info = (FrameInfo*)blk;
				uint8_t *dY = blk + infoAligned;
				uint8_t *dU = dY + ySize;
				uint8_t *dV = dU + cSize;

				rmemcpy(dY, pic.samples[0], ySize);
				rmemcpy(dU, pic.samples[1], cSize);
				rmemcpy(dV, pic.samples[2], cSize);

				info->physY   = (uint32_t)dY + workerPhys;
				info->physU   = (uint32_t)dU + workerPhys;
				info->physV   = (uint32_t)dV + workerPhys;
				info->width   = pic.width_Y;
				info->height  = pic.height_Y;
				info->strideY = pic.stride_Y;
				info->strideC = pic.stride_C;

				PUB_FLUSH(info, sizeof(FrameInfo));
				PUB_FLUSH(dY, ySize);
				PUB_FLUSH(dU, cSize);
				PUB_FLUSH(dV, cSize);
				PUB_FRAME_FENCE();
				PUB_FENCE();

				QUEUE_RESULT(tskPtr, 0xF1A11E, 1, (uint32_t)info + workerPhys, 1);
				dbgFramesPushed++;
			}

			*resPtr = (res == 0) ? 0xDEADBEEF : (uint32_t)res;
			*donePtr = 1;

			// Heartbeat alle 256 NALs
			if ((++dbgNalTotal & 0xFF) == 0) {
				rprintf(tskPtr,
				        "alive nal=%d inH=%d inT=%d rH=%d rT=%d pushed=%d",
				        (int)dbgNalTotal, (int)headIdx, (int)tailIdx,
				        (int)tskPtr->resRing.headIdx,
				        (int)tskPtr->resRing.tailIdx,
				        (int)dbgFramesPushed);
				PUB_FENCE();
			}

			headIdx = (headIdx + 1) % (*maxItemPtr);
			*headIdxPtr = headIdx;
			PUB_FLUSH((void*)headIdxPtr, 4);
			PUB_FENCE();
		} else {
			asm volatile("pause" ::: "memory");
		}
	}
	return;
}
