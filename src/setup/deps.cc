// SPDX-License-Identifier: MIT
// Rust: src/setup/deps.rs — shared helpers.
#include "agentenv/setup/deps.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"

namespace agentenv {
namespace setup {
namespace deps {
namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;
namespace process = core::process;

}  // namespace

core::Expected<std::string, std::string> DetectArch() {
#if defined(__x86_64__)
    return std::string("x86_64");
#elif defined(__aarch64__)
    return std::string("aarch64");
#else
    // Rust reports `std::env::consts::ARCH`; there is no portable equivalent,
    // so the message says what it can.
    return core::make_unexpected(
        std::string("unsupported architecture: this binary was not built for x86_64 or aarch64"));
#endif
}

core::Expected<Unit, std::string> SetFileMode(const std::string& path, uint32_t mode) {
    const core::Expected<Unit, std::string> applied = fs::SetPermissions(path, mode);
    if (!applied.ok()) {
        return core::make_unexpected(std::string("set permissions on ") + path + ": " +
                                     applied.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> SetExecutable(const std::string& path) {
    return SetFileMode(path, 0755);
}

core::Expected<Unit, std::string> CopyFile(const std::string& source,
                                           const std::string& destination, bool executable) {
    const Optional<std::string> parent = fs::Parent(destination);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create target dir '") + *parent + "': " +
                                         made.error());
        }
    }

    const core::Expected<Unit, std::string> copied = fs::Copy(source, destination);
    if (!copied.ok()) {
        return core::make_unexpected(std::string("copy '") + source + "' to '" + destination +
                                     "': " + copied.error());
    }
    if (executable) return SetExecutable(destination);
    return Unit();
}

bool FileExistsNonEmpty(const std::string& path) {
    if (!fs::IsFile(path)) return false;
    const core::Expected<uint64_t, std::string> size = fs::FileSize(path);
    return size.ok() && size.value() > 0;
}

core::Expected<Unit, std::string> DownloadFile(const std::string& url,
                                               const std::string& destination) {
    if (FileExistsNonEmpty(destination)) {
        AGENTENV_DEBUG("already present, skipping download path=" << destination);
        return Unit();
    }

    const Optional<std::string> parent = fs::Parent(destination);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) return core::make_unexpected(made.error());
    }

    AGENTENV_INFO("downloading url=" << url << " dest=" << destination);

    // Rust writes to `dest.with_extension("tmp")` and renames, so a partial
    // transfer never appears under the final name. Same here.
    const std::string temporary = destination + ".tmp";
    fs::RemoveFile(temporary);

    std::vector<std::string> argv;
    argv.push_back("curl");
    argv.push_back("--fail");           // non-2xx becomes a non-zero exit
    argv.push_back("--location");       // follow redirects, as reqwest does
    argv.push_back("--silent");
    argv.push_back("--show-error");
    argv.push_back("--output");
    argv.push_back(temporary);
    argv.push_back(url);

    const core::Expected<process::Output, std::string> output = process::Run(argv);
    if (!output.ok()) {
        fs::RemoveFile(temporary);
        return core::make_unexpected(std::string("GET ") + url + ": " + output.error());
    }
    if (!output.value().success()) {
        fs::RemoveFile(temporary);
        std::ostringstream oss;
        // `--fail` turns an HTTP error into exit 22, which is the closest
        // analogue of Rust's `HTTP {status} for {url}`.
        oss << "GET " << url << " failed (" << output.value().StatusString() << ")";
        const std::string stderr_text = output.value().stderr_text;
        if (!stderr_text.empty()) oss << ": " << stderr_text;
        return core::make_unexpected(oss.str());
    }

    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        const int saved = errno;
        fs::RemoveFile(temporary);
        return core::make_unexpected(std::string("moving downloaded file to destination: ") +
                                     std::strerror(saved));
    }
    return Unit();
}

core::Expected<Unit, std::string> DownloadExecutableFile(const std::string& url,
                                                         const std::string& destination) {
    const core::Expected<Unit, std::string> downloaded = DownloadFile(url, destination);
    if (!downloaded.ok()) return downloaded;
    return SetExecutable(destination);
}

core::Expected<Unit, std::string> ExtractTarGz(const std::string& archive_path,
                                               const std::string& destination) {
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(destination);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create extraction dir '") + destination +
                                     "': " + made.error());
    }

    std::vector<std::string> argv;
    argv.push_back("tar");
    argv.push_back("-xzf");
    argv.push_back(archive_path);
    argv.push_back("-C");
    argv.push_back(destination);

    const core::Expected<process::Output, std::string> output = process::Run(argv);
    if (!output.ok()) {
        return core::make_unexpected(std::string("extract overlaybd package ") + archive_path +
                                     ": " + output.error());
    }
    if (!output.value().success()) {
        std::ostringstream oss;
        oss << "extract overlaybd package " << archive_path << " ("
            << output.value().StatusString() << ")";
        if (!output.value().stderr_text.empty()) oss << ": " << output.value().stderr_text;
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

core::Expected<Unit, std::string> CopyDirRecursive(const std::string& source,
                                                   const std::string& destination) {
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(destination);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create overlaybd lib dir '") + destination +
                                     "': " + made.error());
    }

    const core::Expected<std::vector<std::string>, std::string> entries = fs::ReadDir(source);
    if (!entries.ok()) {
        return core::make_unexpected(std::string("read overlaybd lib dir '") + source + "': " +
                                     entries.error());
    }

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const std::string& name = entries.value()[i];
        const std::string entry_path = fs::Join(source, name);
        const std::string destination_path = fs::Join(destination, name);

        if (fs::IsDir(entry_path)) {
            const core::Expected<Unit, std::string> nested =
                CopyDirRecursive(entry_path, destination_path);
            if (!nested.ok()) return nested;
        } else {
            const core::Expected<Unit, std::string> copied =
                CopyFile(entry_path, destination_path, false);
            if (!copied.ok()) return copied;
        }
    }
    return Unit();
}

core::Expected<Unit, std::string> SetGroupOwner(const std::string& path, uint32_t gid) {
    // -1 for the uid leaves the owner untouched, matching `chown(path, None,
    // Some(gid))`.
    if (::chown(path.c_str(), static_cast<uid_t>(-1), static_cast<gid_t>(gid)) != 0) {
        std::ostringstream oss;
        oss << "set runtime group ownership on " << path << ": " << std::strerror(errno);
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

}  // namespace deps
}  // namespace setup
}  // namespace agentenv
