// Android performance hints (ADPF, Android 13+): the game's busy threads in a
// performance hint session, told each frame how long their work took against
// the frame's time (16.7 ms at 60 fps). The phone then raises the CPU's clocks
// before a frame runs late, instead of after (setting android_perf_hints).
#pragma once

namespace svr2011 {

#if defined(__ANDROID__)
// The calling thread joins the session (once per thread; any thread).
void PerfHintThread();
// Once a frame, on the frame's thread (native_renderer OnPresent): the
// thread's CPU time since the last call is reported as the frame's work.
void PerfHintFrame(int fps);
#else
inline void PerfHintThread() {}
inline void PerfHintFrame(int) {}
#endif

}  // namespace svr2011
