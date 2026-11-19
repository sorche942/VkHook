#ifndef LOGGER_HPP_INCLUDED
#define LOGGER_HPP_INCLUDED

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace VkHook
{

    enum class LogLevel : uint32_t
    {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4,
        None  = 5,
    };

    // The log target has two possible owners: stdout and stderr belong to the C
    // runtime and must survive the Logger, while a file stream is ours to close.
    // Which one applies is carried by the deleter rather than the pointer type,
    // so a single member covers both cases. A struct instead of a std::function
    // keeps the write path free of an indirect call, and unlike a bare function
    // pointer it lets the owning unique_ptr default-construct. It lives at
    // namespace scope because unique_ptr's default constructor will not accept a
    // nested deleter type.
    struct LogStreamDeleter
    {
        bool owned = false;

        void operator()(std::ostream* stream) const noexcept
        {
            if (owned)
                delete stream;
        }
    };

    // Process-wide log. The instance is created on first use and never
    // destroyed, so a log line from a static destructor, an atexit handler or a
    // detached thread during teardown is always safe. Every line carries the
    // pid and the thread id, which is what makes a multi-process (Proton) or
    // multi-threaded log readable.
    //
    // Messages arrive as string_view and are only formatted (timestamp, pid,
    // tid) after the level check. A caller that must also avoid *building* the
    // message passes a callable, or uses the VKHOOK_LOG_* macros below.
    class Logger
    {
    public:
        static void trace(std::string_view message) { log(LogLevel::Trace, message); }
        static void debug(std::string_view message) { log(LogLevel::Debug, message); }
        static void info(std::string_view message) { log(LogLevel::Info, message); }
        static void warn(std::string_view message) { log(LogLevel::Warn, message); }
        static void err(std::string_view message) { log(LogLevel::Error, message); }

        // The callable builds the message and only runs when the level is on.
        template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<std::string, F>>>
        static void trace(F&& build) { log(LogLevel::Trace, std::forward<F>(build)); }
        template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<std::string, F>>>
        static void debug(F&& build) { log(LogLevel::Debug, std::forward<F>(build)); }

        static void log(LogLevel level, std::string_view message)
        {
            if (isLevelEnabled(level))
                instance().emit(level, message);
        }

        template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<std::string, F>>>
        static void log(LogLevel level, F&& build)
        {
            if (isLevelEnabled(level))
                instance().emit(level, build());
        }

        static LogLevel logLevel()
        {
            return static_cast<LogLevel>(instance().minLevel.load(std::memory_order_relaxed));
        }

        static void setLogLevel(LogLevel level)
        {
            instance().minLevel.store(static_cast<uint32_t>(level), std::memory_order_relaxed);
        }

        // Cheap level check for lazy logging macros: one relaxed atomic load.
        static bool isLevelEnabled(LogLevel level)
        {
            return static_cast<uint32_t>(level) >= instance().minLevel.load(std::memory_order_relaxed);
        }

        // Get level name string
        static const char* levelName(LogLevel level);

    private:
        Logger();
        ~Logger() = default;  // never run: the instance is leaked on purpose

        static Logger& instance();

        void emit(LogLevel level, std::string_view message);

        std::atomic<uint32_t> minLevel;

        std::mutex mutex;  // serialises writes to the stream

        // see LogStreamDeleter above: owned=false for the borrowed standard
        // streams, owned=true for the file stream this Logger opened
        std::unique_ptr<std::ostream, LogStreamDeleter> outStream;

        static LogLevel getMinLogLevel();
    };

} // namespace VkHook

// Lazy-evaluated logging macros. Use these on hot paths (per-frame, per-draw)
// to avoid paying for std::string concatenation when the corresponding log
// level is filtered out. The condition is checked once; the message stream
// is only built if the level is enabled.
//
//   VKHOOK_LOG_DEBUG([&](std::string& s) { s = "foo=" + std::to_string(x); });
//
#define VKHOOK_LOG_TRACE(msg_fn) \
    do { if (::VkHook::Logger::isLevelEnabled(::VkHook::LogLevel::Trace)) { \
        std::string _m; msg_fn(_m); ::VkHook::Logger::trace(_m); } } while (false)
#define VKHOOK_LOG_DEBUG(msg_fn) \
    do { if (::VkHook::Logger::isLevelEnabled(::VkHook::LogLevel::Debug)) { \
        std::string _m; msg_fn(_m); ::VkHook::Logger::debug(_m); } } while (false)
#define VKHOOK_LOG_INFO(msg_fn) \
    do { if (::VkHook::Logger::isLevelEnabled(::VkHook::LogLevel::Info)) { \
        std::string _m; msg_fn(_m); ::VkHook::Logger::info(_m); } } while (false)
#define VKHOOK_LOG_WARN(msg_fn) \
    do { if (::VkHook::Logger::isLevelEnabled(::VkHook::LogLevel::Warn)) { \
        std::string _m; msg_fn(_m); ::VkHook::Logger::warn(_m); } } while (false)

#endif // LOGGER_HPP_INCLUDED
