// SPDX-License-Identifier: MIT
// Pins the Docker credential search to an empty directory for the duration of
// a test.
//
// `record_catalog::Delete` now rolls source-registry publications back through
// the production `AcrDiskImageExporter`, which reads
// `$DOCKER_CONFIG/config.json` and `$HOME/.docker/config.json`. Left alone, a
// test would inherit whatever the developer's or CI machine's Docker config
// says -- and a config with a `credsStore` makes the loader *fork a credential
// helper*. That is a unit test whose behaviour depends on the host, and one
// that can hang on a helper waiting for a keyring.
//
// Pointing both variables at an empty directory makes the answer always
// "no credentials": no file to read, no helper to spawn, and the rollback
// fails with `MissingCredentials`, which is the case these tests want anyway.
#ifndef AGENTENV_TESTS_NO_DOCKER_CREDENTIALS_H_
#define AGENTENV_TESTS_NO_DOCKER_CREDENTIALS_H_

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace agentenv {
namespace testing {

class NoDockerCredentials {
 public:
    NoDockerCredentials() : had_docker_config_(false), had_home_(false) {
        char templ[] = "/tmp/agentenv-no-docker-XXXXXX";
        // A real empty directory rather than a path that does not exist: the
        // two behave the same for this loader today, but a directory cannot
        // be accidentally created underneath by something else.
        const char* created = ::mkdtemp(templ);
        directory_ = created != 0 ? created : "/nonexistent-agentenv-docker-config";

        Save("DOCKER_CONFIG", &had_docker_config_, &docker_config_);
        Save("HOME", &had_home_, &home_);
        ::setenv("DOCKER_CONFIG", directory_.c_str(), 1);
        // `HOME` too: the loader tries `$DOCKER_CONFIG/config.json` *and*
        // `$HOME/.docker/config.json`, so overriding only the first still
        // leaves the developer's real config in the search path.
        ::setenv("HOME", directory_.c_str(), 1);
    }

    ~NoDockerCredentials() {
        Restore("DOCKER_CONFIG", had_docker_config_, docker_config_);
        Restore("HOME", had_home_, home_);
        ::rmdir(directory_.c_str());
    }

 private:
    NoDockerCredentials(const NoDockerCredentials&);
    NoDockerCredentials& operator=(const NoDockerCredentials&);

    static void Save(const char* name, bool* had, std::string* value) {
        const char* current = ::getenv(name);
        *had = current != 0;
        if (*had) *value = current;
    }

    /// Restored exactly, including "was not set at all": leaving an empty
    /// `HOME` behind would break every later test in the same binary.
    static void Restore(const char* name, bool had, const std::string& value) {
        if (had) {
            ::setenv(name, value.c_str(), 1);
        } else {
            ::unsetenv(name);
        }
    }

    std::string directory_;
    bool had_docker_config_;
    std::string docker_config_;
    bool had_home_;
    std::string home_;
};

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_NO_DOCKER_CREDENTIALS_H_
