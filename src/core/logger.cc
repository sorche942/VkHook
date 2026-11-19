#include "logger.hh"

#include "paths.hh"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <new>
#include <sys/syscall.h>
#include <unistd.h>

namespace VkHook
{
    Logger& Logger::instance()
    {
        // Leaked on purpose; see the class comment. Function-local so the first
        // user, whichever static initialiser that is, constructs it.
        static Logger* logger = new Logger();
        return *logger;
    }

    Logger::Logger() : minLevel(static_cast<uint32_t>(getMinLogLevel()))
    {
        if (static_cast<LogLevel>(minLevel.load()) != LogLevel::None)
        {
            const std::string filename = Paths::logFile();
            if (filename == "stderr")
            {
                outStream = {&std::cerr, {false}};
            }
            else if (filename == "stdout")
            {
                outStream = {&std::cout, {false}};
            }
            else
            {
                std::error_code ec;
                std::filesystem::path path(filename);
                std::filesystem::create_directories(path.parent_path(), ec);

                ec.clear();
                auto fileSize = std::filesystem::file_size(path, ec);
                if (!ec && fileSize > 4 * 1024 * 1024)
                {
                    std::filesystem::path oldPath = path;
                    oldPath += ".1";
                    std::filesystem::remove(oldPath, ec);
                    ec.clear();
                    std::filesystem::rename(path, oldPath, ec);
                }

                auto file = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app);
                if (*file)
                    outStream = {file.release(), {true}};
                else
                    outStream = {&std::cerr, {false}};
            }
        }
    }

    void Logger::emit(LogLevel level, std::string_view message)
    {
        if (!outStream || level == LogLevel::None)
            return;

        static constexpr std::array<const char*, 5> prefixes = {
            {"VkHook trace: ", "VkHook debug: ", "VkHook info:  ", "VkHook warn:  ", "VkHook err:   "}};
        const char* prefix = prefixes.at(static_cast<uint32_t>(level));

        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm localTime{};
        localtime_r(&time, &localTime);

        char stamp[48];
        const size_t stampLength = std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &localTime);

        static thread_local const long tid = static_cast<long>(syscall(SYS_gettid));
        char origin[64];
        std::snprintf(origin, sizeof(origin), ".%03d [%d:%ld] ", static_cast<int>(milliseconds),
                      static_cast<int>(getpid()), tid);

        // one string per message, one write under the lock: lines from
        // different threads never interleave
        std::string text;
        text.reserve(message.size() + 96);
        size_t begin = 0;
        while (begin <= message.size())
        {
            size_t end = message.find('\n', begin);
            if (end == std::string_view::npos)
                end = message.size();
            if (end > begin || begin == 0)
            {
                text.append(stamp, stampLength).append(origin).append(prefix);
                text.append(message.substr(begin, end - begin)).push_back('\n');
            }
            begin = end + 1;
        }

        std::lock_guard<std::mutex> lock(mutex);
        outStream->write(text.data(), static_cast<std::streamsize>(text.size()));
        outStream->flush();
    }

    const char* Logger::levelName(LogLevel level)
    {
        static constexpr std::array<const char*, 5> names = {{"TRACE", "DEBUG", "INFO", "WARN", "ERROR"}};
        uint32_t idx = static_cast<uint32_t>(level);
        if (idx < names.size())
            return names[idx];
        return "UNKNOWN";
    }

    LogLevel Logger::getMinLogLevel()
    {
        const std::array<std::pair<const char*, LogLevel>, 6> logLevels = {{
            {"trace", LogLevel::Trace},
            {"debug", LogLevel::Debug},
            {"info", LogLevel::Info},
            {"warn", LogLevel::Warn},
            {"error", LogLevel::Error},
            {"none", LogLevel::None},
        }};

        const std::string logLevelStr = Paths::env("VKHOOK_LOG_LEVEL");

        for (const auto& [name, level] : logLevels)
        {
            if (logLevelStr == name)
                return level;
        }

        return LogLevel::Info;
    }

} // namespace VkHook
