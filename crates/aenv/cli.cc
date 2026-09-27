// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/main.rs — the CLI dispatcher (clap-based upstream).
#include "agentenv/aenv/cli.h"
#include "agentenv/aenv/commands.h"

#include <cstdio>
#include <string>
#include <vector>

namespace agentenv {
namespace aenv {

static int CmdHelp() {
    std::printf(
        "aenv — AgentENV client (C++11 skeleton)\n\n"
        "USAGE:\n"
        "  aenv <command> [args...]\n\n"
        "COMMANDS:\n"
        "  list | ls                list sandboxes\n"
        "  start <template>         create + boot a sandbox\n"
        "  exec <id> -- <cmd...>    run a command in a sandbox\n"
        "  connect <id>             attach an interactive shell\n"
        "  delete <id>              delete a sandbox\n"
        "  pause <id>               pause a sandbox\n"
        "  resume <id>              resume a sandbox\n"
        "  snapshot <id>            snapshot a sandbox\n"
        "  timeout <id> <secs>      update a sandbox timeout\n"
        "  build <context>          build a template\n"
        "  pull <image>             pull an image\n"
        "  template list|delete     manage templates\n"
        "  upload <id> <l> <r>      upload a file\n"
        "  download <id> <r> <l>    download a file\n"
        "  auth login|logout|status manage credentials\n"
        "  version                  print version\n"
        "  help                     this message\n");
    return 0;
}

int Main(int argc, char** argv) {
    if (argc < 2) return CmdHelp();
    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return CmdHelp();
    if (cmd == "version") { std::printf("aenv 0.1.0 (C++11 skeleton)\n"); return 0; }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);

    commands::Context ctx;
    // A real build reads these from env / config (AENV_API, AENV_TOKEN).
    ctx.api_endpoint = "http://127.0.0.1:6767";

    int rc = commands::Dispatch(cmd, ctx, args);
    if (rc == -1) {
        std::fprintf(stderr, "aenv: unknown command '%s' (try 'aenv help')\n", cmd.c_str());
        return 2;
    }
    return rc;
}

}  // namespace aenv
}  // namespace agentenv
