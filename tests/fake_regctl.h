// SPDX-License-Identifier: MIT
// Rust: the `fixture` module of src/snapshot/image_export/regctl.rs.
//
// A real executable standing in for `regctl`, rather than an injected functor.
// That is the point: `BlobPipe` is almost entirely fork/pipe/dup2/waitpid, so
// a fake that replaced the process layer would test none of it. With a script
// on disk the kernel really does move the bytes between two children, and the
// `blob put` side really does verify the digest -- which is what makes a
// truncated transfer detectable in the test rather than only in production.
//
// The script logs every argv it is called with, so the *choice* of operation
// (copy vs pipe vs put) can be asserted, not just the end state.
#ifndef AGENTENV_TESTS_FAKE_REGCTL_H_
#define AGENTENV_TESTS_FAKE_REGCTL_H_

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agentenv {
namespace testing {

/// A throwaway directory holding the fake binary, its log, and its state.
class FakeRegctl {
 public:
    FakeRegctl() {
        char templ[] = "/tmp/agentenv-fake-regctl-XXXXXX";
        const char* created = ::mkdtemp(templ);
        dir_ = created != 0 ? created : "/tmp/agentenv-fake-regctl-fallback";
        binary_ = dir_ + "/regctl";
        Install();
    }

    ~FakeRegctl() {
        // `rm -rf` by shell: the tree is nested and this is test-only
        // scaffolding, so a hand-rolled recursive unlink would be more code
        // than the thing it cleans up.
        const std::string command = "rm -rf '" + dir_ + "'";
        if (::system(command.c_str()) != 0) {
            // Nothing useful to do; a leaked temp dir under /tmp is not worth
            // failing a passing test over.
        }
    }

    const std::string& dir() const { return dir_; }
    const std::string& binary() const { return binary_; }

    /// Every `argv:` line the fake logged, in call order.
    std::vector<std::string> ArgvLines() const {
        std::vector<std::string> lines;
        std::ifstream log((dir_ + "/log").c_str());
        std::string line;
        while (std::getline(log, line)) {
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            lines.push_back(line);
        }
        return lines;
    }

    void ClearLog() const { ::unlink((dir_ + "/log").c_str()); }

    bool Logged(const std::string& expected) const {
        const std::vector<std::string> lines = ArgvLines();
        for (std::vector<std::string>::size_type i = 0; i < lines.size(); ++i) {
            if (lines[i] == "argv:" + expected) return true;
        }
        return false;
    }

    std::string BlobPath(const std::string& repository, const std::string& digest) const {
        return dir_ + "/state/blobs/" + repository + "/" + digest;
    }

    bool BlobExists(const std::string& repository, const std::string& digest) const {
        struct stat st;
        return ::stat(BlobPath(repository, digest).c_str(), &st) == 0;
    }

    std::string ReadBlob(const std::string& repository, const std::string& digest) const {
        return ReadFile(BlobPath(repository, digest));
    }

    void SeedBlob(const std::string& repository, const std::string& digest,
                  const std::string& bytes) const {
        const std::string path = BlobPath(repository, digest);
        MakeDirs(path.substr(0, path.rfind('/')));
        WriteFile(path, bytes);
    }

    /// The fake's manifest key: `/`, `:` and `@` all become `_`.
    static std::string ManifestKey(const std::string& image_ref) {
        std::string key = image_ref;
        for (std::string::size_type i = 0; i < key.size(); ++i) {
            if (key[i] == '/' || key[i] == ':' || key[i] == '@') key[i] = '_';
        }
        return key;
    }

    std::string ManifestDir(const std::string& image_ref) const {
        return dir_ + "/state/manifests/" + ManifestKey(image_ref);
    }

    void SeedManifest(const std::string& image_ref, const std::string& manifest_digest) const {
        const std::string store = ManifestDir(image_ref);
        MakeDirs(store);
        WriteFile(store + "/digest", manifest_digest + "\n");
    }

    std::string StoredManifestBody(const std::string& image_ref) const {
        return ReadFile(ManifestDir(image_ref) + "/body");
    }

    std::string StoredManifestDigest(const std::string& image_ref) const {
        std::string digest = ReadFile(ManifestDir(image_ref) + "/digest");
        while (!digest.empty() &&
               (digest[digest.size() - 1] == '\n' || digest[digest.size() - 1] == '\r')) {
            digest.erase(digest.size() - 1);
        }
        return digest;
    }

    /// Makes the next `n` invocations of `op` fail transiently, so the retry
    /// loop can be observed. Counted down by the script itself.
    void FailTransiently(const std::string& op, int times) const {
        MakeDirs(dir_ + "/state");
        std::ostringstream value;
        value << times;
        WriteFile(dir_ + "/state/fail_" + op, value.str() + "\n");
    }

    static std::string ReadFile(const std::string& path) {
        std::ifstream in(path.c_str(), std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

 private:
    FakeRegctl(const FakeRegctl&);
    FakeRegctl& operator=(const FakeRegctl&);

    static void MakeDirs(const std::string& path) {
        const std::string command = "mkdir -p '" + path + "'";
        if (::system(command.c_str()) != 0) {
        }
    }

    static void WriteFile(const std::string& path, const std::string& bytes) {
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void Install() const {
        // Upstream's fake, plus a `fail_<op>` countdown so the retry/backoff
        // path is reachable. `nf` prints the `[http 404]` shape that
        // `RegctlStderrIsNotFound` matches -- the whole point of routing a 404
        // differently from a transient failure.
        const char* script =
            "#!/usr/bin/env bash\n"
            "dir=\"$(cd \"$(dirname \"$0\")\" && pwd)\"; state=\"$dir/state\"\n"
            "printf 'argv:%s\\n' \"$*\" >>\"$dir/log\"\n"
            "nf() { echo \"regctl: $* [http 404]: not found\" >&2; exit 1; }\n"
            "mkey() { printf '%s' \"$1\" | tr '/:@' '___'; }\n"
            "fail_once() {\n"
            "  f=\"$state/fail_$1\"; [ -f \"$f\" ] || return 1\n"
            "  n=$(cat \"$f\"); [ \"$n\" -gt 0 ] || return 1\n"
            "  echo $((n - 1)) >\"$f\"\n"
            "  echo \"regctl: $1 temporary failure [http 503]\" >&2; return 0\n"
            "}\n"
            "case \"$1 $2\" in\n"
            "\"blob head\") fail_once bhead && exit 1\n"
            "  [ -f \"$state/blobs/$3/$4\" ] || nf \"blob $3 $4\" ;;\n"
            "\"blob get\") fail_once bget && exit 1\n"
            "  [ -f \"$state/blobs/$3/$4\" ] || nf \"blob $3 $4\"; cat \"$state/blobs/$3/$4\" ;;\n"
            "\"blob put\") fail_once bput && exit 1\n"
            "  mkdir -p \"$state/blobs/$3\"; cat >\"$state/blobs/$3/$5\" ;;\n"
            "\"blob copy\") fail_once bcopy && exit 1\n"
            "  [ -f \"$state/blobs/$3/$5\" ] || nf \"blob $3 $5\"\n"
            "  mkdir -p \"$state/blobs/$4\"; cp \"$state/blobs/$3/$5\" \"$state/blobs/$4/$5\" ;;\n"
            "\"manifest head\") fail_once mhead && exit 1\n"
            "  m=\"$state/manifests/$(mkey \"$3\")\"; [ -f \"$m/digest\" ] || nf \"manifest $3\"\n"
            "  cat \"$m/digest\" ;;\n"
            "\"manifest put\") fail_once mput && exit 1\n"
            "  m=\"$state/manifests/$(mkey \"$5\")\"; mkdir -p \"$m\"; cat >\"$m/body\"\n"
            "  printf 'sha256:%s\\n' \"$(sha256sum \"$m/body\" | cut -d' ' -f1)\" >\"$m/digest\" ;;\n"
            "*) echo \"unexpected regctl invocation: $*\" >&2; exit 2 ;;\n"
            "esac\n";
        WriteFile(binary_, script);
        ::chmod(binary_.c_str(), 0755);
    }

    std::string dir_;
    std::string binary_;
};

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_FAKE_REGCTL_H_
