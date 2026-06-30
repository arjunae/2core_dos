#ifndef TWOCORE_H
#define TWOCORE_H

#pragma GCC optimize ("-fno-strict-aliasing")
#pragma GCC optimize ("-fno-toplevel-reorder")

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <dos.h>
#include <sys/farptr.h>
#include <go32.h>
#include <pc.h>
#include <signal.h>

enum class ApStatus : uint32_t {
	Init     = 0,
	Booting  = 1,
	Ready    = 2,
	Finished = 3
};

enum class ApCmd : uint32_t {
	None  = 0,
	Spawn = 1,
	Stop  = 2
};

struct ResultSlot {
	uint32_t ticketId;
	uint32_t readyFlag;
	uint32_t msgVal;
	char     dbgStr[64];
} __attribute__((packed, aligned(4)));

#define RESULT_RING_SLOTS 32 

struct ResultRing {
	volatile uint32_t headIdx;
	volatile uint32_t tailIdx;
	uint32_t          maxItem;
	uint32_t dataArrPhys;
	ResultSlot	  dataArr[RESULT_RING_SLOTS];
} __attribute__((packed, aligned(4)));


struct TaskInt {
	volatile ApCmd    cmd;              // offset   0
	volatile ApStatus status;           // offset   4
	volatile uint32_t input;            // offset   8
	volatile uint32_t result;           // offset  12
	volatile uint32_t tick;             // offset  16
	volatile uint32_t funcPtr;          // offset  20
	volatile uint32_t ready;            // offset  24
	volatile uint32_t msgPtr;           // offset  28
	volatile uint32_t ticketId;         // offset  32
	volatile char     debugString[512]; // offset  36
	volatile uint32_t intEax;           // offset 548
	volatile uint32_t intEbx;           // offset 552
	volatile uint32_t intEcx;           // offset 556
	volatile uint32_t intEdx;           // offset 560
	volatile uint32_t intEsi;           // offset 564
	volatile uint32_t intEdi;           // offset 568
	volatile uint32_t intFlg;           // offset 572
	volatile uint32_t numInt;           // offset 576
	volatile uint32_t reqInt;           // offset 580
	ResultRing        resRing;          // offset 584
} __attribute__((packed, aligned(4)));

extern "C" void* loadBin(const char* filePath, uint32_t* physAddr);
void cpu2Main();
uint32_t logPhys(void *logPtr);
int mapMem(uint32_t physAddress);
void microWait(uint32_t waitUs);
void milliWait(uint32_t waitMs);
uint32_t initMem();
uint32_t allocXMS(uint32_t size_bytes, uint16_t* outHandle = nullptr);
void catchSig(int sigNum);
uint32_t scanLen(void *startPtr);

class CoreTwoAPI {
public:
	uint32_t physAddress;
	uint32_t alignPhysical;
	uint8_t trampolinPage;
	int apicSel;
	TaskInt *taskPtr;

	uint32_t memoryBase;
	uint32_t taskOff;
	uint32_t commandOff;
	uint32_t statusOff;
	uint32_t inputOff;
	uint32_t resultOff;
	uint32_t tickOff;
	uint32_t functionOff;
	uint32_t rdyState;
	uint32_t msgPtrO;
	uint32_t dbgStrOff;
	uint32_t payloadBase;
	uint32_t dataOff;

	uint32_t startAp(uint8_t apicId = 1);
	void spawn(uint32_t blockStart);
	void pollDbg(uint32_t flagVal);
	ApStatus status();
	uint32_t getRes();
	uint32_t getTick();
	uint32_t getBiosTick();
	void finish();
	void sendData(void *sourcePointer, uint32_t dataSize);
	void handlePendingInterrupts();
	bool checkTaskEvent(uint32_t &outTicket, uint32_t &outReady, uint32_t &outMsg, char* outStr);
	void sendDataOffset(void *sourcePointer, uint32_t dataSize, uint32_t offset);
	uint32_t getHeadIdx();
	uint32_t getDataOff();
	uint32_t getPayloadBase();
	uint32_t getTramPhys();
	void wakeAp();
	bool setupSys(const char* filePath);
	void recomputeOffsets();
	uint32_t workerPhys = 0;
private:
	void* workerHeap = nullptr;
	uint32_t ticketIdOff;
};

extern CoreTwoAPI core2;
extern CoreTwoAPI *globTask;

#endif
