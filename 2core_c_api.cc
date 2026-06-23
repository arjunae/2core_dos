#include "2core.h"
#include "2core_c_api.h"

extern "C" {

void core2_shutdown(void) {
    core2.finish();
}


bool core2_setupSys(const char* filePath) {
	return core2.setupSys(filePath);
}

uint32_t core2_startAp(uint8_t apicId) {
    return core2.startAp(apicId);
}

void core2_spawn(uint32_t blockStart) {
    core2.spawn(blockStart);
}

void core2_syncHeadIdx(void) {
    extern struct pipeDat myPipe;
    myPipe.ring_buff1.headIdx = core2.getHeadIdx();
}

void core2_sendDataOffset(void *sourcePointer, uint32_t dataSize, uint32_t offset) {
    core2.sendDataOffset(sourcePointer, dataSize, offset);
}

void core2_sendData(void *sourcePointer, uint32_t dataSize) {
    core2.sendData(sourcePointer, dataSize);
}

void core2_handlePendingInterrupts(void) {
    core2.handlePendingInterrupts();
}

uint32_t core2_getTramPhys(void) {
    return core2.memoryBase;
}

bool core2_checkTaskEvent(uint32_t *outTicket, uint32_t *outReady, uint32_t *outMsg, char* outStr) {
    uint32_t t, r, m;
    bool res = core2.checkTaskEvent(t, r, m, outStr);
    if (res) {
        if (outTicket) *outTicket = t;
        if (outReady) *outReady = r;
        if (outMsg) *outMsg = m;
    }
    return res;
}

}

extern "C" void* core2_loadBin(const char* filePath, uint32_t* physAddr) {
    return loadBin(filePath, physAddr);
}

extern "C" uint32_t core2_getPayloadBase(void) {
    return core2.payloadBase;
}

extern "C" void core2_writeLinear(uint32_t linearAddr, void* src, uint32_t size) {
    _dosmemputb(src, size, linearAddr);
}

extern "C" void core2_readLinear(uint32_t linearAddr, void* dst, uint32_t size) {
    _dosmemgetb(linearAddr, size, dst);
}

extern "C" uint32_t core2_getDataOff(void) {
    return core2.dataOff;
}

extern "C" void core2_wakeAp_c(void) {
    core2.wakeAp();
}
