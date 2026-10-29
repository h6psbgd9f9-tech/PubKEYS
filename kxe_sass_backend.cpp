#include "kxe_sass_backend.h"
#ifdef PKH_HAVE_CUDA
#include "minute_speed.h"
#include <cstdlib>
#include <iostream>
#include <mutex>

// The experimental adapter checked the planner and reported candidates, but
// that cannot detect a point/filter error which silently suppresses a match.
// Until independent coverage validation exists, production must not load a
// SASS module or acknowledge any of its work. Keep the former research source
// in the audit backup; no environment variable can bypass this gate.
bool kxe_sass_requested() {
    const char* folder = std::getenv("PKH_KXE_SASS_DIR");
    if (folder && *folder) {
        static std::once_flag warning;
        std::call_once(warning, [] {
            std::lock_guard<std::mutex> lock(g_mtx_cout);
            MinuteSpeed::LogScope status;
            std::cerr << "  Экспериментальный SASS отключён: проверка только кандидатов"
                         " не подтверждает отсутствие пропущенных совпадений.\n"
                         "  Этот участок будет обработан штатным движком CUDA/CPU.\n";
        });
    }
    // The caller keeps the same ordinal and takes its ordinary next_chunk /
    // scan_chunk path. Only that completed path may advance the checkpoint.
    return false;
}

bool kxe_sass_scan(const ChunkIdx& ordinal, KxeSassStats* stats,
                   void (*after_batch)(uint64_t)) {
    (void)ordinal;
    (void)after_batch;
    if (stats) *stats = KxeSassStats{};
    // Direct callers fail closed too: no count/sample/stop/commit/callback,
    // module access, assembly subprocess, CUDA context or kernel launch.
    return false;
}

void kxe_sass_reset() {
    // No experimental context or buffers can be created by this adapter.
}
#endif