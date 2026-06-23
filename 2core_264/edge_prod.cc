
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <conio.h>
#include <sys/time.h>

#include <dpmi.h>
#include <go32.h>
#include <sys/nearptr.h>
#include <sys/farptr.h>
#include <pc.h>

#include "../2core.h"
#include "core_shared.h"
#include "vesa.h"

#define WRK_BIN "edge_wk.bin"

// Each ring slot owns its own NAL buffer so the producer can keep many
// NALs in flight without overwriting a buffer the decoder is still reading
#define RING_SLOTS    32
#define NAL_SLOT_SZ   (256 * 1024)   // max NAL size we forward to CPU2

// Amount of bitstream pushed into the NAL ring before the first frame so CPU2
// can fill the decode pipeline before the display and the FPS clock start.
// caps earlier if 2M would yield more NALs than can be in flight at once
#define PREBUFFER_BYTES  (2 * 1024 * 1024)

unsigned _stklen = 512 * 1024;

#define DBG(...)  do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static const uint8_t *edge264_find_start_code(const uint8_t *buf, const uint8_t *end, int four_byte) {
	while (buf < end - 3) {
		if (buf[0] == 0 && buf[1] == 0 && buf[2] == 1) return buf;
		if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1) return buf + 1;
		buf++;
	}
	return end;
}

// Interrupt-independent timing: gettimeofday relies on the BIOS tick
// (IRQ0, 18.2 Hz) and ticks are lost in the busy-wait spinloops of the
// 2-core build, so it measures too little time. The TSC instead runs at the
// CPU frequency and is unaffected by IRQ0 or a reprogrammed PIT channel 0
static inline uint64_t rdtsc(void) {
	uint32_t lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

// Measure the TSC frequency against the BIOS tick counter at 0x46C, which the
// existing channel 0 ISR increments at 18.2065 Hz. No PIT is reprogrammed, no
// port is altered, so it does not disturb 2CoreDOS. Call this once in a tight
// isolated loop BEFORE CPU2 is spawned; then no ticks are lost here, which
// only happens in the spin loops of the main run
static uint64_t calibrate_tsc(void) {
	uint32_t t = _farpeekl(_dos_ds, 0x46C);
	while (_farpeekl(_dos_ds, 0x46C) == t) { }      // align to a tick edge
	uint32_t startTick = _farpeekl(_dos_ds, 0x46C);
	uint64_t tsc0 = rdtsc();

	const uint32_t WAIT_TICKS = 10;                 // 0.55 s measurement window
	while ((uint32_t)(_farpeekl(_dos_ds, 0x46C) - startTick) < WAIT_TICKS) { }
	uint64_t tsc1 = rdtsc();

	uint32_t elapsedTicks = _farpeekl(_dos_ds, 0x46C) - startTick;
	// Tick period 65536 / 1193182 s
	double seconds = (double)elapsedTicks * 65536.0 / 1193182.0;
	return (uint64_t)((double)(tsc1 - tsc0) / seconds);
}

static void poll_frames(uint32_t taskOff, uint8_t *heapLinPtr, uint32_t heapPhys, uint32_t *lfb, int screen_w, int screen_h, int framedrop, int fps_limit, struct timeval *start_tv, int *frames_decoded, int novid) {
	if (!lfb) return;
	
	uint32_t headIdx = _farpeekl(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.headIdx));
	uint32_t tailIdx = _farpeekl(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.tailIdx));
	uint32_t maxItem = _farpeekl(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.maxItem));
	
	while (tailIdx != headIdx) {
		uint32_t slotBase = taskOff + offsetof(struct TaskInt, resRing.dataArr[0]) + tailIdx * sizeof(struct ResultSlot);
		uint32_t ticketId = _farpeekl(_dos_ds, slotBase + offsetof(struct ResultSlot, ticketId));
		uint32_t readyFlag = _farpeekl(_dos_ds, slotBase + offsetof(struct ResultSlot, readyFlag));
		uint32_t msgVal   = _farpeekl(_dos_ds, slotBase + offsetof(struct ResultSlot, msgVal));
		
		if (ticketId == 0xF1A11E && readyFlag) {
			uint32_t infoPhys = msgVal;
			struct FrameInfo info;
			
			uint32_t infoOffset = infoPhys - heapPhys;
			struct FrameInfo *infoPtr = (struct FrameInfo *)(heapLinPtr + infoOffset);
			
			info = *infoPtr;
			
			if (info.width == 0 || info.height == 0 || info.width > 4096 || info.height > 4096) {
			} else {
				uint8_t *srcY = heapLinPtr + (info.physY - heapPhys);
				uint8_t *srcU = heapLinPtr + (info.physU - heapPhys);
				uint8_t *srcV = heapLinPtr + (info.physV - heapPhys);
				
				if (!novid && (framedrop <= 0 || (*frames_decoded % (framedrop + 1) == 0))) {
					vesa_draw_yuv420(lfb, srcY, srcU, srcV,
					                 info.width, info.height,
					                 info.strideY, info.strideC,
					                 screen_w, screen_h);
				}
				
				(*frames_decoded)++;
				
				if (fps_limit > 0) {
					struct timeval now;
					double targetTime = *frames_decoded / (double)fps_limit;
					while (1) {
						gettimeofday(&now, NULL);
						double elapsed = (now.tv_sec - start_tv->tv_sec) + ((double)now.tv_usec - (double)start_tv->tv_usec) / 1000000.0;
						if (elapsed >= targetTime) break;
					}
				}
			}
			
			_farpokel(_dos_ds, slotBase + offsetof(struct ResultSlot, readyFlag), 0);
		}
		
		tailIdx = (tailIdx + 1) % maxItem;
		_farpokel(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.tailIdx), tailIdx);
		asm volatile("" ::: "memory");
	}
}


int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage %s <video.264> [screen_w] [screen_h] [--framedrop] [--novideo]\n", argv[0]);
		return 1;
	}

	int screen_w = 1024;
	int screen_h = 768;
	int framedrop = 0;
	int fps_limit = 0;
	int novid = 0;
	char *video_file = NULL;
	
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--framedrop") == 0) {
			framedrop = 1;
		} else if (strcmp(argv[i], "--novideo") == 0) {
			novid = 1;
		} else if (!video_file) {
			video_file = argv[i];
		} else if (screen_w == 1024) {
			screen_w = atoi(argv[i]);
		} else {
			screen_h = atoi(argv[i]);
		}
	}
	
	if (!video_file) {
		return 1;
	}

	if (!__djgpp_nearptr_enable()) {
		return 1;
	}

	if (!core2.setupSys(WRK_BIN)) {
		return 1;
	}

	uint32_t dataOff = core2.getDataOff();
	uint32_t taskOff = core2.taskOff;

	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, headIdx), 0);
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx), 0);
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, maxItem), RING_SLOTS);

	uint32_t heapPhys = 256 * 1024 * 1024;
	uint32_t heapSize = 64 * 1024 * 1024;

	uint32_t nalRegionSz = (uint32_t)RING_SLOTS * NAL_SLOT_SZ;   
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, heapPtr),  heapPhys + nalRegionSz);
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, heapSize), heapSize - nalRegionSz);

	__dpmi_meminfo mem;
	mem.address = heapPhys;
	mem.size = heapSize;
	if (__dpmi_physical_address_mapping(&mem) != 0) {
		return 1;
	}
	uint8_t *heapLinPtr = (uint8_t *)(mem.address + __djgpp_conventional_base);
	

	for (int i = 0; i < RING_SLOTS; i++)
		_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, slots[0].done) + i * sizeof(struct NalSlot), 0);

	_farpokel(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.headIdx), 0);
	_farpokel(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.tailIdx), 0);
	_farpokel(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.maxItem), 32);

	FILE *f = fopen(video_file, "rb");
	if (!f) {
		return 1;
	}
	fseek(f, 0, SEEK_END);
	uint32_t fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	vesa_dump_modes(screen_w, screen_h);

	uint32_t* lfb = vesa_init(screen_w, screen_h);
	if (!lfb) {
		fclose(f);
		return 1;
	}

	// Store the LFB physical address and geometry for the AP; the AP uses them to set a WC MTRR over the framebuffer. The BSP cannot write MSRs under HDPMI32, and this must happen BEFORE the core2 spawn
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, vesa_lfb),    vesa_phys_base);
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, vesa_pitch),  vesa_pitch);
	_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, vesa_height), screen_h);

	uint64_t tsc_hz = calibrate_tsc();

	{ uint32_t rc = core2.startAp(2); }

	core2.spawn(core2.workerPhys);

	uint32_t read_buf_sz = 16 * 1024 * 1024;
	uint8_t *file_buf = (uint8_t *)malloc(read_buf_sz);
	if (!file_buf) {
		vesa_restore_text_mode();
		fclose(f);
		return 1;
	}
	
	int jobItr = 0;
	int frames_decoded = 0;
	struct timeval start_tv;
	
	uint32_t bytes_in_buf = fread(file_buf, 1, read_buf_sz, f);
	const uint8_t *nal = file_buf;
	const uint8_t *end0 = file_buf + bytes_in_buf;
	nal += 3 + (nal[2] == 0);

	int reached_eof = 0;

	// Prebuffer stage: push up to PREBUFFER_BYTES of bitstream into the NAL ring
	// and let CPU2 fill resRing BEFORE anything is drawn and the clock runs.
	// We deliberately do NOT poll here (so nothing is drawn); the decoded frames
	// stay queued in resRing and are output first at playback start, so nothing
	// is lost. The loop caps itself when the input ring is full (full pipeline)
	// so it never waits on a drain that only the display loop performs -> no deadlock
	{
		uint32_t prebuf_bytes = 0;
		while (prebuf_bytes < PREBUFFER_BYTES) {
			const uint8_t *end = edge264_find_start_code(nal, end0, 0);

			if (end == end0 && !feof(f)) {
				uint32_t unparsed_len = end0 - nal;
				memmove(file_buf, nal, unparsed_len);
				uint32_t read_bytes = fread(file_buf + unparsed_len, 1, read_buf_sz - unparsed_len, f);
				bytes_in_buf = unparsed_len + read_bytes;
				nal = file_buf;
				end0 = file_buf + bytes_in_buf;
				continue;
			}

			uint32_t nal_sz = end - nal;
			if (nal_sz == 0 && feof(f)) { reached_eof = 1; break; }   // whole file fits in the prebuffer

			if (nal_sz > NAL_SLOT_SZ) {
				nal = end + 3;
				continue;
			}

			uint32_t maxItem = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, maxItem));
			uint32_t tailIdx = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx));
			uint32_t nxtTail = (tailIdx + 1) % maxItem;

			// Input ring full: the pipeline is filled as far as possible without
			// drawing; break here instead of blocking, playback takes over
			if (nxtTail == _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, headIdx)))
				break;

			uint8_t  *nalDst  = heapLinPtr + (size_t)tailIdx * NAL_SLOT_SZ;
			uint32_t  nalPhys = heapPhys   + tailIdx * NAL_SLOT_SZ;
			memcpy(nalDst, nal, nal_sz);

			uint32_t slotOff = dataOff + offsetof(struct EdgeData, slots[0]) + tailIdx * sizeof(struct NalSlot);
			_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, ptr_phys), nalPhys);
			_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, size),     nal_sz);
			_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, done),     0);

			asm volatile("" ::: "memory");
			_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx), nxtTail);

			jobItr++;
			prebuf_bytes += nal_sz;
			nal = end + 3;

			if (end == end0 && feof(f)) { reached_eof = 1; break; }
		}
	}

	// Clock starts with playback, after prebuffering, so the FPS measures the
	// actual display rather than the prefill
	uint64_t tsc_start = rdtsc();
	gettimeofday(&start_tv, NULL);

	while (!reached_eof) {
		if (kbhit()) {
			int c = getch();
			if (c == 27 || c == 'q' || c == 'Q') break;
		}

		poll_frames(taskOff, heapLinPtr, heapPhys, lfb, screen_w, screen_h, framedrop, fps_limit, &start_tv, &frames_decoded, novid);

		const uint8_t *end = edge264_find_start_code(nal, end0, 0);

		if (end == end0 && !feof(f)) {
			uint32_t unparsed_len = end0 - nal;
			memmove(file_buf, nal, unparsed_len);
			uint32_t read_bytes = fread(file_buf + unparsed_len, 1, read_buf_sz - unparsed_len, f);
			bytes_in_buf = unparsed_len + read_bytes;
			nal = file_buf;
			end0 = file_buf + bytes_in_buf;
			continue;
		}

		uint32_t nal_sz = end - nal;
		if (nal_sz == 0 && feof(f)) {
			reached_eof = 1;
			break;
		}
		
		if (nal_sz > NAL_SLOT_SZ) {
			nal = end + 3;
			continue;
		}

		uint32_t maxItem = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, maxItem));
		uint32_t tailIdx = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx));
		uint32_t nxtTail = (tailIdx + 1) % maxItem;

		while (nxtTail == _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, headIdx))) {
			poll_frames(taskOff, heapLinPtr, heapPhys, lfb, screen_w, screen_h, framedrop, fps_limit, &start_tv, &frames_decoded, novid);
			core2.handlePendingInterrupts();
			asm volatile("pause" ::: "memory");
		}

		uint8_t  *nalDst  = heapLinPtr + (size_t)tailIdx * NAL_SLOT_SZ;
		uint32_t  nalPhys = heapPhys   + tailIdx * NAL_SLOT_SZ;
		memcpy(nalDst, nal, nal_sz);

		uint32_t slotOff = dataOff + offsetof(struct EdgeData, slots[0]) + tailIdx * sizeof(struct NalSlot);
		_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, ptr_phys), nalPhys);
		_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, size),     nal_sz);
		_farpokel(_dos_ds, slotOff + offsetof(struct NalSlot, done),     0);

		asm volatile("" ::: "memory");
		_farpokel(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx), nxtTail);

		jobItr++;
		nal = end + 3;
		if (end == end0 && feof(f)) {
			reached_eof = 1;
			break;
		}
	}

	if (reached_eof) {
		uint32_t maxItem = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, maxItem));
		uint32_t tailIdx = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx));
		uint32_t lastTail = (tailIdx == 0) ? (maxItem - 1) : (tailIdx - 1);
		uint32_t lastOffset = dataOff + offsetof(struct EdgeData, slots[0]) + lastTail * sizeof(struct NalSlot);

		while (1) {
			poll_frames(taskOff, heapLinPtr, heapPhys, lfb, screen_w, screen_h, framedrop, fps_limit, &start_tv, &frames_decoded, novid);
			core2.handlePendingInterrupts();
			
			uint32_t inHead = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, headIdx));
			uint32_t inTail = _farpeekl(_dos_ds, dataOff + offsetof(struct EdgeData, tailIdx));
			uint32_t resHead  = _farpeekl(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.headIdx));
			uint32_t resTail  = _farpeekl(_dos_ds, taskOff + offsetof(struct TaskInt, resRing.tailIdx));
			
			int isDone = (inHead == inTail && resHead == resTail);
			if (jobItr > 0) {
				uint32_t doneFlag = _farpeekl(_dos_ds, lastOffset + offsetof(struct NalSlot, done));
				if (!doneFlag) isDone = 0;
			}
			
			if (isDone) break;
			asm volatile("pause" ::: "memory");
		}
	}

	uint64_t tsc_end = rdtsc();

	struct timeval endTv;
	gettimeofday(&endTv, NULL);

	vesa_restore_text_mode();

	fclose(f);
	free(file_buf);

	double elapsed = (tsc_hz > 0)
	               ? (double)(tsc_end - tsc_start) / (double)tsc_hz
	               : (endTv.tv_sec - start_tv.tv_sec) + ((double)endTv.tv_usec - (double)start_tv.tv_usec) / 1000000.0;
	double avgFps = 0.0;
	if (elapsed > 0.0) {
		avgFps = frames_decoded / elapsed;
	}

	char dbgStr[64];
	for (int cIdx = 0; cIdx < 64; cIdx++) {
		dbgStr[cIdx] = _farpeekb(_dos_ds, taskOff + offsetof(struct TaskInt, debugString) + cIdx);
		if (dbgStr[cIdx] == '\0') break;
	}
	dbgStr[63] = '\0';

	printf("\n==========================================\n");
	printf("\nJobs %d Frames %d\n", jobItr, frames_decoded);
	printf("Time %.2f s FPS %.2f\n", elapsed, avgFps);
	printf("CPU2 Log %s\n", dbgStr);
	printf("==========================================\n\n");
	fflush(stdout);

	core2.finish();
	return 0;
}
