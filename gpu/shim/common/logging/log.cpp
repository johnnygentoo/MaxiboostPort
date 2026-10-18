// bbport: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = std::getenv("BB_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
// BB_GPU_LOG_CLASSES=<prefix>[,<prefix>...]: those log classes (e.g. Lib.NpMatching2) log everything
// (trace and debug too) whatever BB_GPU_LOG says.
static bool debug_class(std::string_view log_class) {
    static const char* list = std::getenv("BB_GPU_LOG_CLASSES");
    for (const char* at = list; at && *at;) {
        const char* end = std::strchr(at, ',');
        const size_t n = end ? size_t(end - at) : std::strlen(at);
        if (n && log_class.substr(0, n) == std::string_view(at, n)) return true;
        at = end ? end + 1 : nullptr;
    }
    return false;
}
bool ShouldLog(Level level) { return level >= threshold(); }
bool ShouldLog(std::string_view log_class, Level level) {
    return level >= threshold() || debug_class(log_class);
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
}
} // namespace Common::Log
