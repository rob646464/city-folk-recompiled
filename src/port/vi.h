#pragma once
// Emulated VI retrace interrupt (59.94 Hz NTSC).

namespace port::vi {

void start_retrace_timer();
void stop_retrace_timer();

}  // namespace port::vi
