// SPDX-License-Identifier: MIT
// Rust: src/api/impls/pagination.rs
//
// Keyset pagination over `(time, value)` pairs. The cursor carries the sort
// direction as well as the position, because a token minted for a descending
// listing would silently select the wrong side of the keyset if replayed
// against an ascending one.
#ifndef AGENTENV_API_PAGINATION_H_
#define AGENTENV_API_PAGINATION_H_

#include <algorithm>
#include <string>
#include <vector>

#include "agentenv/core/base64.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/time.h"

namespace agentenv {
namespace api {

// The Rust bounds `T: Display` and `T: for<'a> TryFrom<&'a str>` become these
// two overloads. They are declared before `PaginationCursor` because the
// template calls them unqualified: for a `T` in namespace `std`, ADL would not
// reach `agentenv::api`, so they must be visible at template definition time.

/// Rust's `T: Display`. Overload for a cursor value type.
inline std::string ToStringValue(const std::string& value) { return value; }

/// Rust's `T: TryFrom<&str>`. Overload for a cursor value type.
inline core::Expected<core::Unit, std::string> ParseValue(const std::string& text,
                                                          std::string* out) {
    *out = text;
    return core::Unit();
}

/// Rust `enum PaginationError`.
struct PaginationError {
    enum class Kind {
        DecodeCursor,
        InvalidCursorFormat,
        InvalidCursorUtf8,
        InvalidCursorTimestamp,
        InvalidCursorValue,
    };

    Kind kind = Kind::InvalidCursorFormat;
    std::string detail;

    /// Rust `#[error(..)]` renderings.
    std::string Message() const;

    bool operator==(const PaginationError& o) const {
        return kind == o.kind && detail == o.detail;
    }
    bool operator!=(const PaginationError& o) const { return !(*this == o); }
};

PaginationError MakePaginationError(PaginationError::Kind kind, const std::string& detail);

/// Rust `struct Paginated<T>`.
template <typename Item>
struct Paginated {
    std::vector<Item> items;
    /// Absent when this is the last page; Rust `Option<String>`.
    core::Optional<std::string> next_token;
};

/// Rust `enum Ordering`.
enum class Ordering {
    Less = -1,
    Equal = 0,
    Greater = 1,
};

/// Rust `SystemTime::cmp` / `Ord::cmp`.
template <typename T>
Ordering CompareValues(const T& a, const T& b) {
    if (a < b) return Ordering::Less;
    if (b < a) return Ordering::Greater;
    return Ordering::Equal;
}

/// Rust `struct PaginationCursor<T>`.
///
/// `T` must render through `ToStringValue` and parse through `ParseValue`,
/// standing in for Rust's `Display` and `TryFrom<&str>` bounds.
template <typename T>
class PaginationCursor {
 public:
    PaginationCursor() : descending_(true) {}
    PaginationCursor(const core::SystemTime& time, const T& value, bool descending)
        : time_(time), value_(value), descending_(descending) {}

    /// Rust `new` / `new_descending` / `new_ascending`.
    static PaginationCursor New(const core::SystemTime& time, const T& value,
                                bool descending) {
        return PaginationCursor(time, value, descending);
    }
    static PaginationCursor NewDescending(const core::SystemTime& time, const T& value) {
        return PaginationCursor(time, value, true);
    }
    static PaginationCursor NewAscending(const core::SystemTime& time, const T& value) {
        return PaginationCursor(time, value, false);
    }

    const core::SystemTime& time() const { return time_; }
    const T& value() const { return value_; }
    bool is_descending() const { return descending_; }

    /// Rust `PaginationCursor::encode`.
    ///
    /// The `__asc` suffix is only written for ascending cursors, so an
    /// existing descending token keeps its exact bytes.
    std::string Encode() const {
        std::string raw = core::FormatRfc3339Nanos(time_) + "__" + ToStringValue(value_);
        if (!descending_) raw += "__asc";
        return core::Base64UrlEncode(raw);
    }

    /// Rust `PaginationCursor::parse`.
    static core::Expected<PaginationCursor, PaginationError> Parse(const std::string& token) {
        const core::Expected<std::string, std::string> decoded =
            core::Base64UrlDecode(token);
        if (!decoded.ok()) {
            return core::make_unexpected(
                MakePaginationError(PaginationError::Kind::DecodeCursor, decoded.error()));
        }

        // Rust `split_once("__")`: the *first* separator ends the timestamp,
        // so a value containing `__` stays intact.
        const std::size_t separator = decoded.value().find("__");
        if (separator == std::string::npos) {
            return core::make_unexpected(
                MakePaginationError(PaginationError::Kind::InvalidCursorFormat, ""));
        }
        const std::string timestamp = decoded.value().substr(0, separator);
        const std::string encoded_value = decoded.value().substr(separator + 2);

        // Rust `rsplit_once("__")`: only a trailing `__asc` is meaningful; any
        // other trailing marker is rejected rather than ignored, so a
        // hand-edited token cannot flip the direction silently.
        std::string value_text = encoded_value;
        bool descending = true;
        const std::size_t suffix = encoded_value.rfind("__");
        if (suffix != std::string::npos) {
            const std::string marker = encoded_value.substr(suffix + 2);
            if (marker != "asc") {
                return core::make_unexpected(
                    MakePaginationError(PaginationError::Kind::InvalidCursorFormat, ""));
            }
            value_text = encoded_value.substr(0, suffix);
            descending = false;
        }

        const core::Expected<core::SystemTime, std::string> time =
            core::ParseRfc3339(timestamp);
        if (!time.ok()) {
            return core::make_unexpected(MakePaginationError(
                PaginationError::Kind::InvalidCursorTimestamp, time.error()));
        }

        T value;
        const core::Expected<core::Unit, std::string> parsed = ParseValue(value_text, &value);
        if (!parsed.ok()) {
            return core::make_unexpected(MakePaginationError(
                PaginationError::Kind::InvalidCursorValue, parsed.error()));
        }

        return PaginationCursor(time.value(), value, descending);
    }

    /// Rust `PaginationCursor::paginate` — sorts, then delegates.
    template <typename Item, typename Sort, typename CompareCursor, typename ToCursor>
    Paginated<Item> Paginate(std::vector<Item> items, const core::Optional<uint32_t>& limit,
                             Sort sort, CompareCursor compare_cursor,
                             ToCursor to_cursor) const {
        if (limit.has_value() && *limit == 0) return Paginated<Item>();
        std::sort(items.begin(), items.end(),
                  [&sort](const Item& a, const Item& b) {
                      return sort(a, b) == Ordering::Less;
                  });
        return PaginateSorted(items, limit, compare_cursor, to_cursor);
    }

    /// Rust `PaginationCursor::paginate_sorted`.
    template <typename Item, typename CompareCursor, typename ToCursor>
    Paginated<Item> PaginateSorted(std::vector<Item> items,
                                   const core::Optional<uint32_t>& limit,
                                   CompareCursor compare_cursor, ToCursor to_cursor) const {
        Paginated<Item> page;
        if (limit.has_value() && *limit == 0) return page;

        // Keep only what sorts strictly after the cursor. `Equal` is dropped
        // too, so the item the cursor names is not repeated on the next page.
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (compare_cursor(items[i], *this) == Ordering::Greater) {
                page.items.push_back(items[i]);
            }
        }

        if (limit.has_value()) {
            const std::size_t cap = static_cast<std::size_t>(*limit);
            // Strictly greater: a page that exactly fills the limit is the
            // last one, and must not hand out a token to an empty page.
            if (page.items.size() > cap) {
                page.next_token =
                    core::Optional<std::string>(to_cursor(page.items[cap - 1]).Encode());
                page.items.resize(cap);
            }
        }
        return page;
    }

    /// Rust `PaginationCursor::compare`.
    ///
    /// The value tiebreak is reversed relative to the time order, which is
    /// what makes the keyset walk in one direction without skipping records
    /// that share a timestamp.
    static Ordering Compare(bool descending, const core::SystemTime& a_time, const T& a_value,
                            const core::SystemTime& b_time, const T& b_value) {
        const Ordering time_order = descending ? CompareValues(b_time, a_time)
                                               : CompareValues(a_time, b_time);
        if (time_order != Ordering::Equal) return time_order;
        return descending ? CompareValues(a_value, b_value) : CompareValues(b_value, a_value);
    }

    /// Rust `PaginationCursor::compare_desc`.
    static Ordering CompareDesc(const core::SystemTime& a_time, const T& a_value,
                                const core::SystemTime& b_time, const T& b_value) {
        return Compare(true, a_time, a_value, b_time, b_value);
    }

    bool operator==(const PaginationCursor& o) const {
        return time_ == o.time_ && value_ == o.value_ && descending_ == o.descending_;
    }
    bool operator!=(const PaginationCursor& o) const { return !(*this == o); }

 private:
    core::SystemTime time_;
    T value_;
    bool descending_;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_PAGINATION_H_
