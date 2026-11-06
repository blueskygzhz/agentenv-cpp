// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd/user.rs — Dockerfile `USER` -> a guest account name.
//
// envd resolves every filesystem operation against a named account, while
// Docker also permits a bare UID or a `uid:gid` pair. This module closes that
// gap: it maps the requested form onto a name that exists in the guest,
// generating one when the identity has no account, and it does so without
// rewriting any account the image already has.
//
// The whole file is byte-oriented on purpose. `/etc/passwd` and `/etc/group`
// are not required to be UTF-8 -- GECOS comments and home paths in real images
// are not -- so delimiters and ids are parsed as bytes and only the *selected*
// account's name and home are converted. Converting eagerly would reject
// images that work fine under Docker, and converting lossily would hand envd
// an account name that does not match the file.
#include "agentenv/sandbox/envd.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace sandbox {

const char* const kEnvdBusyboxPath = "/agentenv/bin/busybox";

// Rust `include_str!("add_passwd_entry.sh")`, byte for byte.
//
// Why a lock at all: an image's init system may run `usermod`, which rewrites
// the passwd file through a temporary copy and a rename. An unlocked append
// racing that is silently lost -- the account appears to be created, envd then
// cannot find it, and the failure surfaces as every later SDK call failing on
// an unrelated file operation. So, like shadow's commonio, the script takes
// `PASSWD_FILE.lock` by hard-linking a file holding its PID, and treats a lock
// whose owner has exited as stale.
const char* const kAddPasswdEntryScript =
    "# Append a passwd(5) record under shadow-utils' link lock.\n"
    "#\n"
    "# Usage: sh -c \"$SCRIPT\" sh BUSYBOX PASSWD_FILE ENTRY\n"
    "#\n"
    "# Image init systems may run usermod, which rewrites the passwd file through a\n"
    "# temporary copy and rename; an unlocked append racing it is silently lost.\n"
    "# Like shadow's commonio, take PASSWD_FILE.lock by hard-linking a file holding\n"
    "# our PID, and treat a lock whose owner has exited as stale.\n"
    "set -eu\n"
    "bb=$1\n"
    "passwd=$2\n"
    "entry=$3\n"
    "lock=\"$passwd.lock\"\n"
    "tmp=\"$passwd.$$\"\n"
    "\n"
    "printf '%s' \"$$\" >\"$tmp\"\n"
    "tries=0\n"
    "until \"$bb\" ln \"$tmp\" \"$lock\" 2>/dev/null; do\n"
    "    owner=$(\"$bb\" cat \"$lock\" 2>/dev/null || true)\n"
    "    if [ -n \"$owner\" ] && ! \"$bb\" kill -0 \"$owner\" 2>/dev/null; then\n"
    "        \"$bb\" rm -f \"$lock\"\n"
    "        continue\n"
    "    fi\n"
    "    tries=$((tries + 1))\n"
    "    if [ \"$tries\" -ge 50 ]; then\n"
    "        \"$bb\" rm -f \"$tmp\"\n"
    "        echo \"timed out waiting for $lock\" >&2\n"
    "        exit 1\n"
    "    fi\n"
    "    \"$bb\" sleep 0.1\n"
    "done\n"
    "\"$bb\" rm -f \"$tmp\"\n"
    "trap '\"$bb\" rm -f \"$lock\"' EXIT\n"
    "\n"
    "if ! \"$bb\" grep -qxF \"$entry\" \"$passwd\"; then\n"
    "    printf '\\n%s\\n' \"$entry\" >>\"$passwd\"\n"
    "fi\n"
    "\"$bb\" grep -qxF \"$entry\" \"$passwd\"\n";

namespace {

/// Rust `is_numeric` — non-empty and every byte an ASCII digit.
///
/// Deliberately stricter than the id parser below: `+7` parses as 7 but is not
/// numeric, so it is treated as an account *name* and looked up in the file,
/// exactly as upstream does.
bool IsNumeric(const std::string& value) {
    if (value.empty()) return false;
    for (std::string::size_type i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

/// Rust `std::str::from_utf8(bytes).is_ok()`.
///
/// Expressed through the existing lossy converter rather than as a second
/// UTF-8 validator: the lossy form is the identity on valid input, and on
/// invalid input it must differ (its output is valid UTF-8 while the input is
/// not). One implementation of the UTF-8 rules, so the two cannot drift.
bool IsValidUtf8(const std::string& bytes) { return core::FromUtf8Lossy(bytes) == bytes; }

/// Rust `parse_id` — a `u32` that is not `u32::MAX`.
///
/// `u32::MAX` is excluded because `(uid_t)-1` is the "no change" sentinel in
/// the setuid family and `chown`; letting it through would produce a passwd
/// record whose id means "leave it alone" to anything that later reads it.
///
/// A leading `+` is accepted because Rust's `u32::from_str` accepts one, and
/// this parser is also applied to fields that come from the guest's file
/// rather than from the request.
bool ParseId(const std::string& value, uint32_t* out, std::string* error) {
    const std::string digits =
        (!value.empty() && value[0] == '+') ? value.substr(1) : value;
    bool ok = !digits.empty();
    for (std::string::size_type i = 0; ok && i < digits.size(); ++i) {
        if (digits[i] < '0' || digits[i] > '9') ok = false;
    }
    if (!ok) {
        // Covers a non-UTF-8 input too: Rust reaches `from_utf8` first, but
        // its context string is this same message, and no non-UTF-8 byte is a
        // digit, so the observable error is identical.
        *error = "invalid Dockerfile user/group ID";
        return false;
    }
    uint64_t acc = 0;
    for (std::string::size_type i = 0; i < digits.size(); ++i) {
        acc = acc * 10 + static_cast<uint64_t>(digits[i] - '0');
        if (acc > 0xFFFFFFFFULL) {
            *error = "invalid Dockerfile user/group ID";
            return false;
        }
    }
    if (acc == 0xFFFFFFFFULL) {
        *error = "invalid Dockerfile user/group ID: " + digits;
        return false;
    }
    *out = static_cast<uint32_t>(acc);
    return true;
}

/// Rust `bytes.split(|&b| b == delim)` — n+1 pieces for n delimiters, and
/// exactly one (empty) piece for an empty input.
///
/// That last case is load-bearing: an absent `/etc/group` arrives here as an
/// empty buffer, which must still yield one record so that a request naming
/// the empty group (`USER 0:`) is refused as a malformed record rather than as
/// a missing one.
std::vector<std::string> SplitAll(const std::string& input, char delim) {
    std::vector<std::string> out;
    std::string::size_type start = 0;
    for (;;) {
        const std::string::size_type at = input.find(delim, start);
        if (at == std::string::npos) {
            out.push_back(input.substr(start));
            return out;
        }
        out.push_back(input.substr(start, at - start));
        start = at + 1;
    }
}

std::string Decimal(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", value);
    return std::string(buf);
}

}  // namespace

bool EnvdUserNeedsResolution(const std::string& user) {
    return user.find(':') != std::string::npos || IsNumeric(user);
}

core::Expected<ResolvedUser, std::string> ResolveEnvdUser(const std::string& user,
                                                          const std::string& passwd,
                                                          const std::string& groups) {
    // Rust `user.split_once(':')` — the *first* colon only, so `0:0:0` names
    // the group `0:0` and is refused by the group lookup rather than being
    // quietly truncated to `0:0`.
    const std::string::size_type colon = user.find(':');
    const std::string account = colon == std::string::npos ? user : user.substr(0, colon);
    const bool has_group = colon != std::string::npos;
    const std::string group = has_group ? user.substr(colon + 1) : std::string();

    std::string error;
    bool has_numeric_uid = false;
    uint32_t numeric_uid = 0;
    if (IsNumeric(account)) {
        if (!ParseId(account, &numeric_uid, &error)) return core::make_unexpected(error);
        has_numeric_uid = true;
    }

    // Rust enumerates *before* filtering, so the reported line number is the
    // real one in the file an operator will open -- blank lines and comments
    // included.
    std::vector<std::vector<std::string> > accounts;
    const std::vector<std::string> passwd_lines = SplitAll(passwd, '\n');
    for (std::vector<std::string>::size_type index = 0; index < passwd_lines.size(); ++index) {
        const std::string& line = passwd_lines[index];
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        const std::vector<std::string>::size_type line_number = index + 1;
        const std::vector<std::string> fields = SplitAll(line, ':');
        if (fields.size() != 7 || fields[0].empty()) {
            return core::make_unexpected(
                "malformed /etc/passwd record at line " + Decimal(static_cast<uint32_t>(line_number)) +
                ": expected a name and seven fields");
        }
        uint32_t ignored = 0;
        if (!ParseId(fields[2], &ignored, &error)) {
            return core::make_unexpected("invalid /etc/passwd UID at line " +
                                         Decimal(static_cast<uint32_t>(line_number)));
        }
        if (!ParseId(fields[3], &ignored, &error)) {
            return core::make_unexpected("invalid /etc/passwd GID at line " +
                                         Decimal(static_cast<uint32_t>(line_number)));
        }
        accounts.push_back(fields);
    }

    // The match is by uid when one was given and by name otherwise. Matching
    // by uid is what makes `USER 0` resolve to whatever the image calls root.
    const std::vector<std::string>* existing = NULL;
    for (std::vector<std::vector<std::string> >::size_type i = 0; i < accounts.size(); ++i) {
        const std::vector<std::string>& fields = accounts[i];
        bool match;
        if (has_numeric_uid) {
            uint32_t uid = 0;
            std::string ignored_error;
            match = ParseId(fields[2], &uid, &ignored_error) && uid == numeric_uid;
        } else {
            match = fields[0] == account;
        }
        if (match) {
            existing = &accounts[i];
            break;
        }
    }

    uint32_t uid = 0;
    if (has_numeric_uid) {
        uid = numeric_uid;
    } else {
        if (existing == NULL) {
            return core::make_unexpected(
                std::string("Dockerfile USER names an account absent from /etc/passwd"));
        }
        if (!ParseId((*existing)[2], &uid, &error)) return core::make_unexpected(error);
    }

    uint32_t gid = 0;
    if (has_group && IsNumeric(group)) {
        if (!ParseId(group, &gid, &error)) return core::make_unexpected(error);
    } else if (has_group) {
        // Note the single-field record an empty buffer yields: it compares
        // equal to an empty group name, and is then refused by the arity
        // check below rather than reported as an absent group.
        const std::vector<std::string> group_lines = SplitAll(groups, '\n');
        const std::vector<std::string>* entry = NULL;
        std::vector<std::vector<std::string> > parsed;
        parsed.reserve(group_lines.size());
        for (std::vector<std::string>::size_type i = 0; i < group_lines.size(); ++i) {
            parsed.push_back(SplitAll(group_lines[i], ':'));
        }
        for (std::vector<std::vector<std::string> >::size_type i = 0; i < parsed.size(); ++i) {
            if (parsed[i][0] == group) {
                entry = &parsed[i];
                break;
            }
        }
        if (entry == NULL) {
            return core::make_unexpected(
                std::string("Dockerfile USER names a group absent from /etc/group"));
        }
        if (entry->size() != 4) {
            return core::make_unexpected("malformed /etc/group record for " + group);
        }
        if (!ParseId((*entry)[2], &gid, &error)) return core::make_unexpected(error);
    } else if (existing != NULL) {
        if (!ParseId((*existing)[3], &gid, &error)) return core::make_unexpected(error);
    } else {
        gid = 0;
    }

    // No explicit group and the account already has exactly this primary
    // group: the image's own account is usable as-is. This is the only branch
    // that returns an image-provided name, and it is why `USER 0` does not
    // append anything.
    if (!has_group && existing != NULL) {
        uint32_t existing_gid = 0;
        if (!ParseId((*existing)[3], &existing_gid, &error)) {
            return core::make_unexpected(error);
        }
        if (existing_gid == gid) {
            if (!IsValidUtf8((*existing)[0])) {
                return core::make_unexpected(
                    std::string("selected /etc/passwd account name is not UTF-8"));
            }
            ResolvedUser resolved;
            resolved.name = (*existing)[0];
            return resolved;
        }
    }

    // A separate name preserves explicit UID:GID pairs without rewriting an
    // existing user's primary group. Reuse it on subsequent snapshot restores.
    const std::string home = existing == NULL ? std::string("/") : (*existing)[5];
    if (!IsValidUtf8(home)) {
        return core::make_unexpected(std::string("selected /etc/passwd home is not UTF-8"));
    }

    // Rust `names.entry(fields[0]).or_insert(fields)` — the *first* record
    // wins for a duplicated name, which is also what glibc's lookup does.
    std::map<std::string, const std::vector<std::string>*> names;
    for (std::vector<std::vector<std::string> >::size_type i = 0; i < accounts.size(); ++i) {
        names.insert(std::make_pair(accounts[i][0], &accounts[i]));
    }

    const std::string base_name = "aenv-" + Decimal(uid) + "-" + Decimal(gid);
    // `0..=accounts.len()` — one more candidate than there are records, so a
    // free name is guaranteed to exist and the loop cannot fall through.
    for (std::vector<std::vector<std::string> >::size_type suffix = 0;
         suffix <= accounts.size(); ++suffix) {
        const std::string name =
            suffix == 0 ? base_name
                        : base_name + "-" + Decimal(static_cast<uint32_t>(suffix));
        const std::map<std::string, const std::vector<std::string>*>::const_iterator found =
            names.find(name);
        if (found != names.end()) {
            const std::vector<std::string>& fields = *found->second;
            uint32_t found_uid = 0;
            uint32_t found_gid = 0;
            std::string ignored_error;
            // Every field must match. A name whose record differs in *any* of
            // them describes a different identity, and handing it to envd
            // would silently place the sandbox in the wrong group or home.
            if (fields.size() == 7 && fields[1] == "x" &&
                ParseId(fields[2], &found_uid, &ignored_error) && found_uid == uid &&
                ParseId(fields[3], &found_gid, &ignored_error) && found_gid == gid &&
                fields[4].empty() && fields[5] == home && fields[6] == "/bin/sh") {
                ResolvedUser resolved;
                resolved.name = name;
                return resolved;
            }
            continue;
        }
        ResolvedUser resolved;
        resolved.name = name;
        resolved.passwd_entry = core::Optional<std::string>(
            name + ":x:" + Decimal(uid) + ":" + Decimal(gid) + "::" + home + ":/bin/sh");
        return resolved;
    }
    // Unreachable: see the loop bound above.
    return core::make_unexpected(
        std::string("no free account name after inspecting every /etc/passwd entry"));
}

}  // namespace sandbox
}  // namespace agentenv
