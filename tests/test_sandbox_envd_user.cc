// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd/user.rs `mod tests` — every pure-function case, plus
// the shell-script cases run against the real script on the host (upstream
// does the same, standing `env` in for BusyBox applets).
#include "microtest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/sandbox/envd.h"

using namespace agentenv::sandbox;
using agentenv::core::Optional;

namespace {

const char* const kPasswd =
    "root:x:0:0:root:/root:/bin/sh\n"
    "nonroot:x:65532:65532::/home/nonroot:/sbin/nologin\n";

std::string NameOf(const std::string& user, const std::string& passwd,
                   const std::string& groups) {
    const auto r = ResolveEnvdUser(user, passwd, groups);
    MT_EXPECT_TRUE(r.ok());
    return r.value().name;
}

std::string EntryOf(const std::string& user, const std::string& passwd,
                    const std::string& groups) {
    const auto r = ResolveEnvdUser(user, passwd, groups);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().passwd_entry.has_value());
    return *r.value().passwd_entry;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- needs_resolution ------------------------------------------------------

MT_TEST(needs_resolution_selects_numeric_and_pair_forms) {
    MT_EXPECT_TRUE(EnvdUserNeedsResolution("0"));
    MT_EXPECT_TRUE(EnvdUserNeedsResolution("1000"));
    MT_EXPECT_TRUE(EnvdUserNeedsResolution("nonroot:staff"));
    MT_EXPECT_TRUE(EnvdUserNeedsResolution("0:0"));
    // A plain name is handed to envd untouched -- which is what keeps an
    // ordinary image to a single `POST /init`.
    MT_EXPECT_TRUE(!EnvdUserNeedsResolution("user"));
    MT_EXPECT_TRUE(!EnvdUserNeedsResolution("nonroot"));
    MT_EXPECT_TRUE(!EnvdUserNeedsResolution(""));
    // `+7` parses as a number but is not numeric, so it is a name.
    MT_EXPECT_TRUE(!EnvdUserNeedsResolution("+7"));
}

// ---- resolve_user: Rust's cases, one for one ------------------------------

// Rust `numeric_users_resolve_to_existing_guest_accounts`
MT_TEST(numeric_users_resolve_to_existing_guest_accounts) {
    const char* users[] = {"0", "00", "65532"};
    const char* names[] = {"root", "root", "nonroot"};
    for (int i = 0; i < 3; ++i) {
        const auto r = ResolveEnvdUser(users[i], kPasswd, "");
        MT_EXPECT_TRUE(r.ok());
        MT_EXPECT_EQ(r.value().name, std::string(names[i]));
        // No record to append: the image's own account is used as-is.
        MT_EXPECT_TRUE(!r.value().passwd_entry.has_value());
    }
}

// Rust `missing_numeric_user_keeps_requested_uid_and_gid`
MT_TEST(missing_numeric_user_keeps_requested_uid_and_gid) {
    MT_EXPECT_EQ(EntryOf("1234", kPasswd, ""),
                 std::string("aenv-1234-0:x:1234:0::/:/bin/sh"));

    // And the generated identity is reused verbatim on a later restore,
    // rather than appended a second time.
    const std::string entry = EntryOf("1234:5678", kPasswd, "");
    const std::string updated = std::string(kPasswd) + entry + "\n";
    const auto again = ResolveEnvdUser("1234:5678", updated, "");
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_TRUE(!again.value().passwd_entry.has_value());
    MT_EXPECT_EQ(again.value().name, std::string("aenv-1234-5678"));
}

// Rust `explicit_group_preserves_existing_account_and_home`
MT_TEST(explicit_group_preserves_existing_account_and_home) {
    MT_EXPECT_EQ(EntryOf("nonroot:staff", kPasswd, "staff:x:1000:\n"),
                 std::string("aenv-65532-1000:x:65532:1000::/home/nonroot:/bin/sh"));
}

// Rust `conflicting_generated_name_is_not_reused`
MT_TEST(conflicting_generated_name_is_not_reused) {
    const std::string passwd = std::string(kPasswd) +
                               "other:x:1234:0::/home/other:/bin/sh\n"
                               "aenv-1234-0:x:1234:0::/other:/bin/sh\n";
    // `aenv-1234-0` exists but its home differs, so it describes a different
    // identity and the search moves past it.
    MT_EXPECT_EQ(NameOf("1234:0", passwd, ""), std::string("aenv-1234-0-1"));
}

// Rust `explicit_group_does_not_reuse_existing_account`
MT_TEST(explicit_group_does_not_reuse_existing_account) {
    // An explicit group must never rewrite `nonroot`'s primary group, even
    // though the group named *is* its current one.
    MT_EXPECT_EQ(NameOf("nonroot:65532", kPasswd, ""), std::string("aenv-65532-65532"));
}

// Rust `non_utf8_account_fields_do_not_block_valid_identities`
MT_TEST(non_utf8_account_fields_do_not_block_valid_identities) {
    const std::string passwd =
        std::string("root:x:0:0:r\xff"
                    "ot:/root:/bin/sh\n") +
        "\xff" "user:x:1000:1000::/\xff" "home:/bin/sh\n";
    MT_EXPECT_EQ(NameOf("0", passwd, ""), std::string("root"));

    const std::string groups =
        std::string("\xff") + "group:x:1000:\xff" "user\nstaff:x:1001:\xff" "user\n";
    MT_EXPECT_EQ(EntryOf("0:staff", passwd, groups),
                 std::string("aenv-0-1001:x:0:1001::/root:/bin/sh"));
}

// Rust `malformed_passwd_records_fail_with_line_context`
MT_TEST(malformed_passwd_records_fail_with_line_context) {
    const char* records[] = {
        "nonroot:x:65532:65532::/home/nonroot",  // Missing shell field.
        "nonroot:x:bad:65532::/home/nonroot:/bin/sh",
        "nonroot:x:65532:bad::/home/nonroot:/bin/sh",
        ":x:65532:65532::/home/nonroot:/bin/sh",
    };
    const char* users[] = {"65532", "nonroot:0"};
    for (int i = 0; i < 4; ++i) {
        const std::string passwd =
            std::string("root:x:0:0::/root:/bin/sh\n") + records[i] + "\n";
        for (int u = 0; u < 2; ++u) {
            const auto r = ResolveEnvdUser(users[u], passwd, "");
            MT_EXPECT_TRUE(!r.ok());
            MT_EXPECT_TRUE(Contains(r.error(), "/etc/passwd"));
            // The number is reported to an operator who will open the file.
            MT_EXPECT_TRUE(Contains(r.error(), "line 2"));
        }
    }
}

// Rust `blank_lines_and_comments_do_not_hide_existing_accounts`
MT_TEST(blank_lines_and_comments_do_not_hide_existing_accounts) {
    const std::string passwd = std::string("\n# Accounts\n") + kPasswd + "\n";
    MT_EXPECT_EQ(NameOf("0", passwd, ""), std::string("root"));
}

// Rust `invalid_ids_and_missing_names_fail`
MT_TEST(invalid_ids_and_missing_names_fail) {
    const char* users[] = {
        "4294967295",  // u32::MAX is the "no change" sentinel.
        "4294967296",  // Overflow.
        "0:",          // The empty group is a malformed /etc/group record.
        "0:no-such-group",
        "missing:0",
        ":0",
        "0:0:0",  // The group is `0:0`, which no file names.
    };
    for (int i = 0; i < 7; ++i) {
        MT_EXPECT_TRUE(!ResolveEnvdUser(users[i], kPasswd, "").ok());
    }
}

// ---- mutation checks: each rule above, shown to be load-bearing -----------

MT_TEST(line_numbers_count_blank_and_comment_lines) {
    // Were the enumerate placed after the filter, this would report line 1.
    const std::string passwd = "\n\n# c\nbroken:x:0\n";
    const auto r = ResolveEnvdUser("0", passwd, "");
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "line 4"));
}

MT_TEST(a_malformed_record_is_refused_not_skipped) {
    // Skipping it would hide `root` and append a second uid-0 account.
    const std::string passwd = "root:x:0:0::/root\nnonroot:x:65532:65532::/h:/bin/sh\n";
    MT_EXPECT_TRUE(!ResolveEnvdUser("65532", passwd, "").ok());
}

MT_TEST(an_absent_group_file_refuses_rather_than_defaulting_to_gid_zero) {
    // Reading an absent /etc/group as "no groups" and falling back to gid 0
    // would place the sandbox in root's group.
    const auto r = ResolveEnvdUser("0:staff", kPasswd, "");
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "absent from /etc/group"));
}

MT_TEST(a_generated_name_is_reused_only_on_an_exact_field_match) {
    // Each clause of the reuse test, mutated one at a time: every one of them
    // must push the search to the `-1` suffix.
    //
    // The `seven` account is what pins the expected home. It is not padding:
    // the generated home is inherited from the uid match, so without a
    // separate holder the "wrong home" row would move the *expectation* too
    // and the row would assert nothing.
    const char* conflicting[] = {
        "aenv-7-8:y:7:8::/:/bin/sh",         // password field not `x`
        "aenv-7-8:x:9:8::/:/bin/sh",         // wrong uid
        "aenv-7-8:x:7:9::/:/bin/sh",         // wrong gid
        "aenv-7-8:x:7:8:comment:/:/bin/sh",  // non-empty GECOS
        "aenv-7-8:x:7:8::/other:/bin/sh",    // wrong home
        "aenv-7-8:x:7:8::/:/bin/bash",       // wrong shell
    };
    for (int i = 0; i < 6; ++i) {
        const std::string passwd =
            std::string("seven:x:7:8::/:/bin/sh\n") + conflicting[i] + "\n";
        MT_EXPECT_EQ(NameOf("7:8", passwd, ""), std::string("aenv-7-8-1"));
    }
    // The exact record, by contrast, is reused with nothing to append.
    const auto r = ResolveEnvdUser(
        "7:8", "seven:x:7:8::/:/bin/sh\naenv-7-8:x:7:8::/:/bin/sh\n", "");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().name, std::string("aenv-7-8"));
    MT_EXPECT_TRUE(!r.value().passwd_entry.has_value());
}

MT_TEST(the_first_record_wins_for_a_duplicated_name) {
    // glibc resolves a duplicated name to the first record, so the reuse test
    // must inspect that one. The two `aenv-7-8` records here differ only in
    // their shell, and only the *second* would match: taking the last would
    // reuse the name and hand envd an account whose record is not the one the
    // guest's own lookup will find.
    const std::string passwd =
        "seven:x:7:8::/:/bin/sh\n"
        "aenv-7-8:x:7:8::/:/bin/bash\n"
        "aenv-7-8:x:7:8::/:/bin/sh\n";
    MT_EXPECT_EQ(NameOf("7:8", passwd, ""), std::string("aenv-7-8-1"));
}

MT_TEST(home_is_inherited_from_the_matched_account_and_defaults_to_root_dir) {
    // An explicit uid:gid keeps the image's home ...
    MT_EXPECT_EQ(EntryOf("65532:0", kPasswd, ""),
                 std::string("aenv-65532-0:x:65532:0::/home/nonroot:/bin/sh"));
    // ... and falls back to `/` when the uid matches nothing.
    MT_EXPECT_EQ(EntryOf("4242:0", kPasswd, ""),
                 std::string("aenv-4242-0:x:4242:0::/:/bin/sh"));
}

MT_TEST(a_non_utf8_selected_name_or_home_is_refused) {
    // Returning a lossily-converted name would name no account at all.
    const std::string passwd = std::string("\xff") + "root:x:0:0::/root:/bin/sh\n";
    const auto name = ResolveEnvdUser("0", passwd, "");
    MT_EXPECT_TRUE(!name.ok());
    MT_EXPECT_TRUE(Contains(name.error(), "account name is not UTF-8"));

    const std::string bad_home = std::string("root:x:0:0::/\xff" "home:/bin/sh\n");
    const auto home = ResolveEnvdUser("0:0", bad_home, "");
    MT_EXPECT_TRUE(!home.ok());
    MT_EXPECT_TRUE(Contains(home.error(), "home is not UTF-8"));
}

MT_TEST(only_the_first_colon_splits_the_user) {
    // Splitting on the last would make `0:0:0` mean uid `0:0`, group `0`.
    const auto r = ResolveEnvdUser("0:0:0", kPasswd, "0:x:0:\n");
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(Contains(r.error(), "absent from /etc/group"));
}

MT_TEST(an_explicit_numeric_group_equal_to_the_current_one_still_generates) {
    // The `group.is_none()` guard on the reuse branch is what makes this
    // generate rather than return `nonroot`; dropping it would rewrite the
    // meaning of `USER nonroot:65532` into `USER nonroot`.
    const auto r = ResolveEnvdUser("nonroot:65532", kPasswd, "");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().passwd_entry.has_value());
}

MT_TEST(u32_max_is_rejected_for_both_halves) {
    MT_EXPECT_TRUE(!ResolveEnvdUser("4294967295", kPasswd, "").ok());
    MT_EXPECT_TRUE(!ResolveEnvdUser("0:4294967295", kPasswd, "").ok());
    // ... and in the file, too: a record carrying it must not be trusted.
    MT_EXPECT_TRUE(!ResolveEnvdUser("0", "x:x:4294967295:0::/:/bin/sh\n", "").ok());
}

// ---- add_passwd_entry.sh ---------------------------------------------------
//
// Upstream runs the guest script on the host with `env` standing in for the
// BusyBox applets. Same here: the script is the artifact under test, and the
// lock protocol it implements is the whole reason it exists.

namespace {

class TempDir {
 public:
    TempDir() {
        char tmpl[] = "/tmp/aenv-passwd-XXXXXX";
        const char* made = ::mkdtemp(tmpl);
        MT_EXPECT_TRUE(made != NULL);
        path_ = made;
    }
    ~TempDir() {
        // Best effort; the test asserts on the contents before this runs.
        const std::string cmd = "rm -rf '" + path_ + "'";
        (void)std::system(cmd.c_str());
    }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    MT_EXPECT_TRUE(!out.fail());
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

bool Exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

/// Runs an argv to completion, returning its exit code.
///
/// fork/exec rather than `system()`: the script contains single quotes (the
/// `trap` line), so no amount of shell quoting can pass it through a command
/// string intact -- and a mangled script would be testing something else.
int RunArgv(const std::vector<std::string>& argv, const std::string& stderr_path) {
    const pid_t pid = ::fork();
    MT_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        if (!stderr_path.empty()) {
            const int fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                (void)::dup2(fd, 2);
                ::close(fd);
            }
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (std::vector<std::string>::size_type i = 0; i < argv.size(); ++i) {
            raw.push_back(const_cast<char*>(argv[i].c_str()));
        }
        raw.push_back(NULL);
        ::execvp(raw[0], &raw[0]);
        ::_exit(127);
    }
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// `sh -c "$SCRIPT" sh env PASSWD ENTRY` — the exact argv shape
/// `resolve_default_user` builds, with `env` standing in for BusyBox.
int AddPasswdEntry(const std::string& passwd, const std::string& entry,
                   std::string* stderr_out) {
    const std::string err_path = passwd + ".stderr";
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back(kAddPasswdEntryScript);
    argv.push_back("sh");
    argv.push_back("env");
    argv.push_back(passwd);
    argv.push_back(entry);
    const int code = RunArgv(argv, err_path);
    if (stderr_out != NULL) *stderr_out = ReadFile(err_path);
    (void)::unlink(err_path.c_str());
    return code;
}

int CountLines(const std::string& contents, const std::string& wanted) {
    int count = 0;
    std::istringstream in(contents);
    std::string line;
    while (std::getline(in, line)) {
        if (line == wanted) ++count;
    }
    return count;
}

int64_t MonoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

// Rust `passwd_append_is_idempotent_and_releases_the_lock`
MT_TEST(passwd_append_is_idempotent_and_releases_the_lock) {
    TempDir dir;
    const std::string passwd = dir.path() + "/passwd";
    // A missing trailing newline must not merge records -- which is why the
    // script writes a leading `\n` rather than appending bare.
    WriteFile(passwd, "root:x:0:0::/root:/bin/sh");
    const std::string entry = "aenv-1-2:x:1:2::/:/bin/sh";

    MT_EXPECT_EQ(AddPasswdEntry(passwd, entry, NULL), 0);
    MT_EXPECT_EQ(AddPasswdEntry(passwd, entry, NULL), 0);

    const std::string contents = ReadFile(passwd);
    MT_EXPECT_EQ(CountLines(contents, entry), 1);
    MT_EXPECT_EQ(CountLines(contents, "root:x:0:0::/root:/bin/sh"), 1);
    // No lock or temporary file left behind.
    MT_EXPECT_TRUE(!Exists(passwd + ".lock"));
}

// Rust `passwd_append_reclaims_a_lock_whose_owner_exited`
MT_TEST(passwd_append_reclaims_a_lock_whose_owner_exited) {
    TempDir dir;
    const std::string passwd = dir.path() + "/passwd";
    WriteFile(passwd, "root:x:0:0::/root:/bin/sh\n");
    // A pid that has certainly exited: our own child, reaped.
    const pid_t child = ::fork();
    MT_EXPECT_TRUE(child >= 0);
    if (child == 0) ::_exit(0);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    std::ostringstream pid;
    pid << child;
    WriteFile(passwd + ".lock", pid.str());

    MT_EXPECT_EQ(AddPasswdEntry(passwd, "aenv-1-0:x:1:0::/:/bin/sh", NULL), 0);
    MT_EXPECT_TRUE(!Exists(passwd + ".lock"));
}

// Rust `passwd_append_waits_for_a_live_lock_owner`
MT_TEST(passwd_append_waits_for_a_live_lock_owner) {
    TempDir dir;
    const std::string passwd = dir.path() + "/passwd";
    const std::string lock = passwd + ".lock";
    WriteFile(passwd, "root:x:0:0::/root:/bin/sh\n");
    // Our own pid is certainly live, so `kill -0` succeeds and the lock is
    // not reclaimed as stale -- the append must wait for it to be released.
    std::ostringstream pid;
    pid << ::getpid();
    WriteFile(lock, pid.str());

    std::thread release([&lock]() {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 300L * 1000000L;
        nanosleep(&ts, nullptr);
        (void)::unlink(lock.c_str());
    });

    const int64_t start = MonoMs();
    const int code = AddPasswdEntry(passwd, "aenv-1-0:x:1:0::/:/bin/sh", NULL);
    release.join();

    MT_EXPECT_EQ(code, 0);
    MT_EXPECT_TRUE(MonoMs() - start >= 300);
    MT_EXPECT_TRUE(Contains(ReadFile(passwd), "aenv-1-0:x:1:0::/:/bin/sh"));
}

// Rust `passwd_append_survives_a_locked_concurrent_rewrite`
MT_TEST(passwd_append_survives_a_locked_concurrent_rewrite) {
    TempDir dir;
    const std::string passwd = dir.path() + "/passwd";
    WriteFile(passwd, "root:x:0:0::/root:/bin/sh\n");
    // Rewrite like shadow's usermod: lock, copy, pause, rename, unlock. An
    // unlocked append racing this loses records; the whole script exists to
    // make that impossible.
    std::vector<std::string> rewriter_argv;
    rewriter_argv.push_back("sh");
    rewriter_argv.push_back("-c");
    rewriter_argv.push_back(
        "p=$1; i=0\n"
        "while [ \"$i\" -lt 100 ]; do\n"
        "    echo $$ > \"$p.rw\"\n"
        "    if ln \"$p.rw\" \"$p.lock\" 2>/dev/null; then\n"
        "        cp \"$p\" \"$p.new\"; sleep 0.005; mv \"$p.new\" \"$p\"\n"
        "        rm -f \"$p.lock\"; i=$((i + 1))\n"
        "    fi\n"
        "    rm -f \"$p.rw\"\n"
        "done\n");
    rewriter_argv.push_back("sh");
    rewriter_argv.push_back(passwd);
    int rewriter_code = -1;
    std::thread rewriter([&rewriter_argv, &rewriter_code]() {
        rewriter_code = RunArgv(rewriter_argv, std::string());
    });

    std::vector<std::string> entries;
    for (int uid = 1; uid <= 20; ++uid) {
        std::ostringstream e;
        e << "aenv-" << uid << "-0:x:" << uid << ":0::/:/bin/sh";
        entries.push_back(e.str());
        MT_EXPECT_EQ(AddPasswdEntry(passwd, entries.back(), NULL), 0);
    }
    rewriter.join();
    MT_EXPECT_EQ(rewriter_code, 0);

    const std::string contents = ReadFile(passwd);
    for (std::vector<std::string>::size_type i = 0; i < entries.size(); ++i) {
        // A dropped record is the exact failure the lock prevents, and it is
        // silent without this assertion. Upstream asserts presence only, so
        // this does too -- the rewriter's copy is not ordered against a
        // *second* append of the same entry, which the grep already blocks.
        MT_EXPECT_TRUE(CountLines(contents, entries[i]) >= 1);
    }
}

MT_TEST(passwd_append_gives_up_rather_than_waiting_forever) {
    // 50 tries at 0.1s. A live owner that never releases must make the script
    // fail, not hang: the append runs under a 10 s process timeout upstream,
    // and a hang there would stall sandbox startup instead of reporting.
    TempDir dir;
    const std::string passwd = dir.path() + "/passwd";
    WriteFile(passwd, "root:x:0:0::/root:/bin/sh\n");
    std::ostringstream pid;
    pid << ::getpid();
    WriteFile(passwd + ".lock", pid.str());

    std::string err;
    MT_EXPECT_EQ(AddPasswdEntry(passwd, "aenv-1-0:x:1:0::/:/bin/sh", &err), 1);
    MT_EXPECT_TRUE(Contains(err, "timed out waiting for"));
    // The temp file it used to claim the lock is cleaned up on that path too.
    MT_EXPECT_TRUE(!Contains(ReadFile(passwd), "aenv-1-0"));
}

MT_MAIN
