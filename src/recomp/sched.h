#pragma once
// Guest threads (OSThread) and interrupts.
//
// Every guest thread runs on its own host thread, but only one executes guest
// code at a time: the scheduler hands the "CPU" to the highest-priority ready
// thread, exactly like the console's OS. Switches happen when a thread blocks,
// when it readies a higher-priority thread, or after an interrupt.
//
// Interrupts (VI retrace, DVD/ARQ completion...) are queued by host threads and
// delivered on the running guest thread at its next safe point (loop back-edge
// or SDK call) with interrupts enabled, or by the poster itself when the CPU is
// idle. Handlers run on a dedicated interrupt context and stack.
#include "recomp/ctx.h"

#include <functional>

namespace recomp::sched {

struct ThreadExit {};

// Makes the calling host thread the boot guest thread (OSThread at `guest`).
GuestThread* adoptBootThread(u32 guest, const Ctx& init);
void setIrqContext(const Ctx& base, u32 stackTop);

GuestThread* current();
u32 currentGuest();
Ctx& ctxOf(GuestThread* t);

// OSThread operations; `guest` is the OSThread address.
bool create(u32 guest, u32 entry, u32 arg, u32 stackTop, u32 stackSize, s32 prio, u32 attr);
s32 resume(u32 guest);
s32 suspend(u32 guest);
void cancel(u32 guest);
[[noreturn]] void exitThread(u32 val);
bool isTerminated(u32 guest);
s32 priority(u32 guest);
void detach(u32 guest);

// Blocks the current thread on a thread queue (any guest address works as a key).
void sleepOn(u32 queue);
// Readies every thread blocked on `queue`; may switch to a higher-priority one.
void wakeup(u32 queue);

// The current thread is about to wait for the host (frame handoff, frame.cpp): like a
// blocking OS call it gives up the CPU, so interrupts are taken and other threads run
// meanwhile. reacquireAfterHost() makes it ready again and returns once it owns the CPU.
void releaseForHost();
void reacquireAfterHost();
// Runs host work that does not touch other threads' state (disc reads...) with the CPU
// given up, so a slow host operation does not hold off interrupts and other threads.
template <class F>
void withCpuReleased(F&& work) {
    releaseForHost();
    work();
    reacquireAfterHost();
}

// Queues an interrupt handler (runs with the interrupt context).
void postIrq(std::function<void(Ctx&)> fn);
// Raises the interrupt after `seconds` of host time (models hardware latency).
void postIrqAfter(double seconds, std::function<void(Ctx&)> fn);
bool inIrq();
// The current guest thread runs with interrupts on (MSR[EE]), i.e. not in a critical section.
bool interruptsEnabled();

// Ends every guest thread: each one leaves at its next scheduling point (throws ThreadExit).
void shutdown();

// Starts a detached host thread that runs guest code (guest threads, the boot thread, the
// interrupt timer). Every guest call is a host frame, so these get 8 MiB stacks on POSIX:
// macOS gives std::thread only 512 KiB (Windows: the executable's 1 MiB; Linux: 8 MiB).
void startHostThread(std::function<void()> fn);
// After shutdown(): waits until the created guest threads (not the boot thread) have unwound,
// at most `seconds`; false on timeout.
bool waitForThreads(double seconds);
void dumpThreads();
void serviceOnThread(Ctx& c);

}  // namespace recomp::sched
