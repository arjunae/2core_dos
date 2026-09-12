LIB2CORE - README.txt

This is lib2Core. Its build upon the shoulders of bloodwych's DOS_multicore [demo](https://www.vogons.org/viewtopic.php?t=111336)
which was was extended and embedded within this library. The basic Concept is as follows:

The Host   Core 0: Runs your standard DJGPP application, manages system initialization, handles OS level interrupts, and schedules jobs.
The Worker Core 1: Runs a bare raw binary image entirely dedicated to the computation loop. It has no direct access to standard DOS or DJGPP system calls.

lib2core is following a Producer/Consumer pattern between the two cores:
- main core (BSP) bootstraps the system using core2.setupSys("worker.bin"), wakes the secondary core via its APIC ID using core2.startAp
  and launches the worker binary into execution memory with core2.spawn.
- main core pushes incoming processing data into the shared share_mem ring buffer, advances the tail_idx pointer
- secondary core runs an infinite loop polling the head_idx and queues the finished payloads back into the resRing structure.
- main core concurrently polls the system result queue, processes the calculated data back from high physical memory.

[ Main Core (Host) ]                             [ Secondary Core (Worker) ]
         │                                                    │
  1. Writes data into slot                                    │
  2. Executes memory barrier                                  │
  3. Updates tail_idx ────(Job is now visible)───────────────>│ 4. Detects head_idx != tail_idx
         │                                                    │ 5. Processes the job
         │                                                    │ 
         │                                                    │ 6. Executes QUEUE_RESULT:
         │                                                    │    - Writes result payload
  8. Reads result <───────(Result queued in resRing)──────────│    - Sets readyFlag = 1
  9. Clears readyFlag                                         │
         │                                                    │

Proudly cherished specialties:
The secondary core is extremely fast and will begin polling or executing memory operations the absolute microsecond it leaves the apboot.asm trampoline. Therefore, all shared state configurations must be completely written and committed by the host before the boot command is called
All pointers exchanged inside the shared structures (like share_mem or TaskInt) must be absolute physical addresses.
To access locations sent by the host, the worker must convert it to a relative pointer by subtracting workPhys like [uint32_t rel_adr = targetPhys - workPhys]
The other way around, the host accesses shared high memory buffers by mapping them via DPMI or converting them using the conventional memory base: 
[uint8_t* linear_ptr = (uint8_t*)(phys_addr + __djgpp_conventional_base);]

Notes:
The communication between the Host and the Worker uses standard ring buffers.
When adding a job or a result, always write the payload data before updating the index pointer. You must use PUB_FENCE() 
so its guaranteed that the your data  is committed to RAM and when a core is forced to wait for an index to change then always include the pause instruction inside the spin loop.

The libs Heart has been enhanced in the following ways:

- apboot now dynamically builds a 256 entry Interrupt Descriptor Table. This allows the new apPanicReport function to extract the instruction pointer and error codes insted of silently lock up.
- Its now able to load the worker binary directly into Extended Memory. Conventional memory is preserved exclusively for the tiny real mode boot trampoline.
- floating point unit and SSE instructions are initialized during boot.
- It now loads the physical address of the worker from the configuration slot _patchMain and before transferring control to the worker code, the segment registers DS, ES, and SS are switched to the modified data selector 0x20 so such that  the workers stack functions correctly.
-  A green OK Marker appears when tha AP came up successfully.

aputils.h
Since you cant use stdlib, the memory relevant subset of functions are provided here for convenience: 
rmemcpy , rmemmove , rmemset , rstrlen , rmemcmp , rstrcpy , rstrcmp, rheapInit , rmalloc , rfree
You can even call Interrupts from the AP via CALL_INT() ! The BSP will then send the result back over the bridge

----

A note about using lib2core.a in old codebases:

Beware  of simply offloading a task to the AP while the BSP spins idly is merely asynchronous Execution.
It yields zero processing performance gains and introduces unnecessary cross core communication overhead.
So don't separate the production of jobs and the consumption of results into isolated execution phases or sequential loops.
When the shared ring buffer is temporarily full or empty, the spinning core should not just pause; it should utilize those clock cycles for independent local tasks and finally: 
To achieve a real speedup, the computational complexity of the workload dispatched to the AP must significantly outweigh the overhead of the inter Core synchronization.

Example Apps can be found within 2CoreDOS/mock and 2CoreDOS/2core_264

PS 
lib2core is an educational project. It was created as the result of curiosity regarding the original bloodwych demo.
As apboot is pure assmbler magic, i used claude to understand, extend and test its functionality.
I havent wrote a single line of code in there, so i dont raise any claims on that code and
all credits belong to the original author who invented that magic a century ago. 
Also id like to thank Andreas W and Marc K for reviewing the libraries implementation and for their gcc KungFu. 
Finally, the whole beast is at prototype stage. (Mid2026)
Hey- It was found usable on three machines and virtualbox.

so enjoy debugging- n Dream on
@René Heidisch: Thank you for showing me how to successfully distance myself from bad management that relies on control and restrictions instead of inspiration and responsibility . Free Derry!
