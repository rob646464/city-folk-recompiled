#pragma once
// Host OS helpers used by ported library code.
#ifdef __cplusplus
extern "C" {
#endif

// Names the calling host thread (debugger/profiler visibility only).
void OSSetCurrentThreadName(const char* name);

#ifdef __cplusplus
}
#endif
