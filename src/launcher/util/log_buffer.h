#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <vector>
#include <chrono>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

struct LogEntry {
    std::string message;
    spdlog::level::level_enum level = spdlog::level::info;
    std::string source = "dashboard";
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

class LogBuffer {
public:
    void Push(LogEntry entry) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(std::move(entry));
        if (m_entries.size() > MAX_ENTRIES) {
            m_entries.pop_front();
        }
    }

    void Push(const std::string& msg, spdlog::level::level_enum level = spdlog::level::info,
              const std::string& source = "dashboard") {
        Push(LogEntry{msg, level, source, std::chrono::system_clock::now()});
    }

    std::vector<LogEntry> GetAll() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {m_entries.begin(), m_entries.end()};
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<LogEntry> m_entries;
    static constexpr size_t MAX_ENTRIES = 10000;
};

// Custom spdlog sink that pushes to LogBuffer
template<typename Mutex>
class LogBufferSink : public spdlog::sinks::base_sink<Mutex> {
public:
    explicit LogBufferSink(LogBuffer& buffer, const std::string& source = "dashboard")
        : m_buffer(buffer), m_source(source) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        std::string text = fmt::to_string(formatted);
        // Remove trailing newline if present
        if (!text.empty() && text.back() == '\n') text.pop_back();
        m_buffer.Push(LogEntry{text, msg.level, m_source, msg.time});
    }

    void flush_() override {}

private:
    LogBuffer& m_buffer;
    std::string m_source;
};

using LogBufferSink_mt = LogBufferSink<std::mutex>;
using LogBufferSink_st = LogBufferSink<spdlog::details::null_mutex>;
