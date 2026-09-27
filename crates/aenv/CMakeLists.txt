add_executable(agentenv-aenv
    main.cc
    cli.cc
    client.cc
    util.cc
    commands/mod.cc
    commands/auth.cc
    commands/build.cc
    commands/connect.cc
    commands/delete.cc
    commands/download.cc
    commands/exec.cc
    commands/list.cc
    commands/pause.cc
    commands/pull.cc
    commands/resume.cc
    commands/snapshot.cc
    commands/start.cc
    commands/template.cc
    commands/timeout.cc
    commands/upload.cc
)
target_include_directories(agentenv-aenv PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-aenv PRIVATE agentenv-core)
