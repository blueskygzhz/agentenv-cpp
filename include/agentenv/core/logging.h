// SPDX-License-Identifier: MIT
// Rust: `tracing` + `tracing-subscriber`. C++11: structured, level-based logging
// with a global sink. Keep zero deps; format is deliberately terse.
#ifndef AGENTENV_CORE_LOGGING_H_
#define AGENTENV_CORE_LOGGING_H_

#include <memory>
#include <sstream>
#include <string>

#include "agentenv/core/optional.h"

namespace agentenv {
namespace core {

/// The host logger's severity — Rust `tracing::Level`, which is why `Trace` is
/// here. Not serialised anywhere.
enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error };

/// Rust `logging::LogLevel` — the severity of a *serialised* log record, as it
/// appears in a build log document and in a sandbox log entry.
///
/// Deliberately narrower than `LogLevel` above, and deliberately a separate
/// type rather than an alias. Upstream keeps these two apart for the same
/// reason: `tracing::Level` has `TRACE` and this does not, so folding them
/// together would make a `Trace` record *representable* in a document that has
/// no spelling for it — the writer would accept the entry and the flush would
/// have to invent a level or drop it. The ordering matters too: it backs the
/// `entry.level < requested` filter in a sandbox log query, so the four must
/// stay in ascending severity.
enum class LogRecordLevel : int { Debug = 0, Info, Warn, Error };

/// `"debug" | "info" | "warn" | "error"` — Rust's `rename_all = "lowercase"`.
const char* LogRecordLevelToString(LogRecordLevel level);

/// Exactly those four spellings; anything else is rejected rather than
/// defaulted, so an unknown level in a stored document cannot silently become
/// `Info` and downgrade an error.
Optional<LogRecordLevel> LogRecordLevelFromString(const std::string& text);

class Logger {
 public:
    static Logger& Get();

    void SetLevel(LogLevel lv);
    LogLevel Level() const;

    void Log(LogLevel lv, const char* file, int line, const std::string& msg);

 private:
    Logger();
    LogLevel level_;
};

/// Rust `String::from_utf8_lossy` — every maximal invalid UTF-8 subsequence
/// becomes U+FFFD, exactly as Rust's `Utf8Chunks` delimits them.
std::string FromUtf8Lossy(const std::string& bytes);

/// Rust `str::trim` — strips Unicode `White_Space` (so `\v`, `\f`, U+0085,
/// U+00A0, U+2000..U+200A, U+3000 and friends too) from both ends. `text`
/// must be valid UTF-8, e.g. the output of `FromUtf8Lossy`.
std::string TrimUnicodeWhitespace(const std::string& text);

/// Rust `logging::BuildLogSink` — receives one build's log lines.
class BuildLogSink {
 public:
    virtual ~BuildLogSink() {}
    /// `step` is Rust's `Option<&str>`; null means "no step".
    virtual void Log(LogLevel level, const std::string* step, const std::string& message) = 0;
};

/// Rust `logging::BuildLogger` — writes to a build's sink, or nowhere when
/// it has none (`BuildLogger::default()`). Cheap to copy, like the Rust
/// `Clone` over an `Option<Arc<..>>`.
class BuildLogger {
 public:
    BuildLogger() {}
    explicit BuildLogger(std::shared_ptr<BuildLogSink> sink) : sink_(sink) {}

    void Log(LogLevel level, const std::string* step, const std::string& message) const {
        if (sink_) sink_->Log(level, step, message);
    }

    /// Rust `BuildLogger::output` — process output, one log entry per line
    /// (`str::lines`: split on `\n`, a trailing `\r` stripped, no empty entry
    /// for a final newline), stderr at Warn and stdout at Info.
    void Output(const std::string* step, bool is_stderr, const std::string& bytes) const;

 private:
    std::shared_ptr<BuildLogSink> sink_;
};

}  // namespace core
}  // namespace agentenv

/// The message is **one** streaming expression: `AGENTENV_WARN("a=" << a)`.
///
/// Deliberately a single named parameter rather than `...`. With varargs,
/// `AGENTENV_WARN("a=", a)` expands to `_oss << "a=", a` — the *comma
/// operator*, which evaluates `a` and throws it away. That compiles silently
/// and truncates the message to everything before the first comma, so the one
/// value worth logging is the one that disappears. Four such calls were found
/// in this tree, each dropping the error text from a failure path.
///
/// `-Wunused-value` catches only the subset whose dropped argument has no side
/// effect, so it cannot be relied on. Taking exactly one argument turns every
/// occurrence into "macro passed N arguments but takes just 1" at compile
/// time instead.
#define AGENTENV_LOG(lv, message_expr)                                                   \
    do {                                                                                 \
        auto& _lg = ::agentenv::core::Logger::Get();                                     \
        if (static_cast<int>(_lg.Level()) <= static_cast<int>(lv)) {                     \
            std::ostringstream _oss;                                                     \
            _oss << message_expr;                                                        \
            _lg.Log(lv, __FILE__, __LINE__, _oss.str());                                 \
        }                                                                                \
    } while (0)

#define AGENTENV_TRACE(message_expr) \
    AGENTENV_LOG(::agentenv::core::LogLevel::Trace, message_expr)
#define AGENTENV_DEBUG(message_expr) \
    AGENTENV_LOG(::agentenv::core::LogLevel::Debug, message_expr)
#define AGENTENV_INFO(message_expr) \
    AGENTENV_LOG(::agentenv::core::LogLevel::Info, message_expr)
#define AGENTENV_WARN(message_expr) \
    AGENTENV_LOG(::agentenv::core::LogLevel::Warn, message_expr)
#define AGENTENV_ERROR(message_expr) \
    AGENTENV_LOG(::agentenv::core::LogLevel::Error, message_expr)

#endif  // AGENTENV_CORE_LOGGING_H_
