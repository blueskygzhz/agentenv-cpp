cmake_minimum_required(VERSION 3.16)

# Stand-in VMM for the boot tests: makes the spawn/wait half of the boot
# sequence reachable without KVM.
add_executable(fake_firecracker helpers/fake_firecracker.cc)
set(AGENTENV_FAKE_FC "$<TARGET_FILE:fake_firecracker>")


# Minimal in-source micro-test framework so the skeleton has zero third-party
# test dependency. Real project would migrate this to GoogleTest.
add_library(agentenv-microtest STATIC microtest.cc)
target_include_directories(agentenv-microtest PUBLIC ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(agentenv-microtest PUBLIC agentenv-core)

function(agentenv_add_test NAME SRC)
    add_executable(${NAME} ${SRC})
    target_link_libraries(${NAME} PRIVATE agentenv-microtest ${ARGN})
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

agentenv_add_test(test_expected            test_expected.cc            agentenv-core)
agentenv_add_test(test_identity            test_identity.cc            agentenv-core)
agentenv_add_test(test_json                test_json.cc                agentenv-core)
# Rust: tokio::process::{Command, Child} as used by the image regctl pipeline.
agentenv_add_test(test_core_process        test_core_process.cc        agentenv-core)
agentenv_add_test(test_config              test_config.cc              agentenv-core)
agentenv_add_test(test_toml                test_toml.cc                agentenv-core)
agentenv_add_test(test_cfg                 test_cfg.cc                 agentenv-cfg)
agentenv_add_test(test_volume              test_volume.cc              agentenv-volume)
# Rust: src/volume/catalog.rs
agentenv_add_test(test_volume_catalog      test_volume_catalog.cc      agentenv-volume)
agentenv_add_test(test_template_spec       test_template_spec.cc       agentenv-template)
# Rust: src/template/logs.rs — the bounds and the document, over a real POSIX
# repository so the persistence half is not faked.
agentenv_add_test(test_template_logs       test_template_logs.cc
                  agentenv-template agentenv-snapshot agentenv-volume-record)
# Rust: src/template/step_executor.rs
agentenv_add_test(test_template_step_executor test_template_step_executor.cc agentenv-template)
# Rust: src/template/builder.rs
agentenv_add_test(test_template_builder    test_template_builder.cc    agentenv-template)
# Rust: src/template/builder.rs :: execute_and_publish / build_failure_reason —
# over the real OSS repository on an HTTP stub, so the committed record is what
# gets asserted rather than a recording fake's memory of the metadata.
agentenv_add_test(test_template_publish    test_template_publish.cc
                  agentenv-template agentenv-snapshot agentenv-volume-record agentenv-cfg)
# Rust: src/template/runner.rs
agentenv_add_test(test_template_runner     test_template_runner.cc     agentenv-template)
agentenv_add_test(test_secrets             test_secrets.cc             agentenv-rootmod)
agentenv_add_test(test_local_store         test_local_store.cc         agentenv-rootmod)
agentenv_add_test(test_setup               test_setup.cc               agentenv-rootmod)
agentenv_add_test(test_packages            test_packages.cc            agentenv-rootmod)
agentenv_add_test(test_overlaybd_setup     test_overlaybd_setup.cc     agentenv-rootmod)
# Rust: src/{startup_pack,startup_manifest}.rs — the two startup wire formats
agentenv_add_test(test_startup_pack_codec  test_startup_pack_codec.cc  agentenv-overlaybd)
# Rust: storage/overlaybd/src/io/virtual_file.rs
agentenv_add_test(test_overlaybd_virtual_file test_overlaybd_virtual_file.cc agentenv-overlaybd)
# Rust: storage/overlaybd/src/sys/*.rs
agentenv_add_test(test_overlaybd_sys test_overlaybd_sys.cc agentenv-overlaybd)
# Rust: storage/overlaybd/src/backend/local.rs
agentenv_add_test(test_overlaybd_local_file test_overlaybd_local_file.cc agentenv-overlaybd)
# Rust: storage/overlaybd/src/lsmt/file/readonly.rs
agentenv_add_test(test_overlaybd_lsmt_readonly test_overlaybd_lsmt_readonly.cc agentenv-overlaybd)
# Rust: storage/overlaybd/src/lsmt/file/readwrite.rs
agentenv_add_test(test_overlaybd_lsmt_rw test_overlaybd_lsmt_rw.cc agentenv-overlaybd)
agentenv_add_test(test_overlaybd_lsmt_compact test_overlaybd_lsmt_compact.cc agentenv-overlaybd)
agentenv_add_test(test_overlaybd_bk_download test_overlaybd_bk_download.cc agentenv-overlaybd)
# Rust: the write path of src/backend/cache/full_file_cache/cache_store.rs.
agentenv_add_test(test_overlaybd_cache_write test_overlaybd_cache_write.cc agentenv-overlaybd)
# Rust: the open path and eviction tiers of
# src/backend/cache/full_file_cache/cache_pool.rs.
agentenv_add_test(test_overlaybd_cache_open test_overlaybd_cache_open.cc agentenv-overlaybd)
# Rust: the ZFile read metrics of src/metrics.rs and the pread sampler of
# src/compression/zfile.rs.
agentenv_add_test(test_overlaybd_zfile_metrics test_overlaybd_zfile_metrics.cc agentenv-overlaybd)
# Rust: src/dense_export.rs
agentenv_add_test(test_overlaybd_dense_export test_overlaybd_dense_export.cc agentenv-overlaybd)
# Rust: src/tools/oci.rs
agentenv_add_test(test_overlaybd_tools_oci test_overlaybd_tools_oci.cc agentenv-overlaybd)
# Rust: src/tools/packaging.rs + lsmt/file/helper.rs::create_mappings_from_sparse
agentenv_add_test(test_overlaybd_tools_packaging test_overlaybd_tools_packaging.cc
                  agentenv-overlaybd)
agentenv_add_test(test_overlaybd_image_file test_overlaybd_image_file.cc agentenv-overlaybd)
agentenv_add_test(test_deps                test_deps.cc                agentenv-rootmod)
# Rust: deps/mod.rs :: prepare_tools_image + sandbox/tools_image.rs — which of
# the two tools-drive shapes a launch gets, and the manifest discovery the
# provisioning steps need.
agentenv_add_test(test_setup_tools_image   test_setup_tools_image.cc   agentenv-rootmod)
agentenv_add_test(test_overlaybd_p2p       test_overlaybd_p2p.cc       agentenv-overlaybd-p2p)
agentenv_add_test(test_mpsc_queue          test_mpsc_queue.cc          agentenv-core)
agentenv_add_test(test_dto                 test_dto.cc                 agentenv-api)
agentenv_add_test(test_mock_backend        test_mock_backend.cc        agentenv-sandbox)
agentenv_add_test(test_sandbox_process     test_sandbox_process.cc     agentenv-sandbox)
# Rust: src/sandbox/runtime_versions.rs — the probes that feed the `--version`
# parsers in snapshot/version.cc.
agentenv_add_test(test_sandbox_runtime_versions test_sandbox_runtime_versions.cc agentenv-sandbox)
agentenv_add_test(test_sandbox_envd        test_sandbox_envd.cc        agentenv-sandbox)
# Rust: src/sandbox/logs.rs — decoding plus the bounded query page.
agentenv_add_test(test_sandbox_logs        test_sandbox_logs.cc        agentenv-sandbox)
# Rust: src/sandbox/metrics.rs + src/orchestrator/sandbox_metrics.rs — the
# envd-boundary validation and the retention cache's two clocks.
agentenv_add_test(test_sandbox_metrics     test_sandbox_metrics.cc
                  agentenv-sandbox agentenv-orchestrator)
# Rust: src/sandbox/guest_volumes.rs — in-guest mount/sync/freeze ordering.
agentenv_add_test(test_sandbox_guest_volumes test_sandbox_guest_volumes.cc agentenv-sandbox)
# Rust: src/snapshot/publish.rs + store_snapshot/delete_snapshot of store.rs
agentenv_add_test(test_snapshot_publish    test_snapshot_publish.cc    agentenv-snapshot)
# Rust: src/snapshot/image_export/acr/{source_image,client,publisher}.rs — the
# manifest-delete half a snapshot delete needs.
agentenv_add_test(test_snapshot_acr        test_snapshot_acr.cc        agentenv-snapshot)
# Rust: src/snapshot/image_export/regctl.rs — driven against a real fake
# `regctl` script, so the fork/pipe half of `BlobPipe` is actually exercised.
agentenv_add_test(test_snapshot_regctl     test_snapshot_regctl.cc     agentenv-snapshot)
# Rust: src/snapshot/image_export/service.rs — end to end over the fake regctl,
# so the transfer-shape choices are asserted on argv.
agentenv_add_test(test_snapshot_image_service test_snapshot_image_service.cc agentenv-snapshot)
# Rust: src/sandbox/envd/user.rs — Dockerfile USER -> a guest account name
agentenv_add_test(test_sandbox_envd_user   test_sandbox_envd_user.cc   agentenv-sandbox)
agentenv_add_test(test_custom_extension    test_custom_extension.cc    agentenv-sandbox)
agentenv_add_test(test_sandbox_access      test_sandbox_access.cc      agentenv-sandbox agentenv-cfg)
agentenv_add_test(test_sandbox_manifest    test_sandbox_manifest.cc    agentenv-sandbox)
agentenv_add_test(test_sandbox_network_policy test_sandbox_network_policy.cc agentenv-sandbox)
agentenv_add_test(test_sandbox_network_slot test_sandbox_network_slot.cc agentenv-sandbox)
agentenv_add_test(test_sandbox_network_resolver test_sandbox_network_resolver.cc agentenv-sandbox agentenv-cfg Threads::Threads)
agentenv_add_test(test_sandbox_egress_proxy test_sandbox_egress_proxy.cc agentenv-sandbox Threads::Threads)
agentenv_add_test(test_sandbox_network_manager test_sandbox_network_manager.cc agentenv-sandbox Threads::Threads)
agentenv_add_test(test_sandbox_firecracker test_sandbox_firecracker.cc agentenv-sandbox Threads::Threads)
agentenv_add_test(test_sandbox_fc_pool     test_sandbox_fc_pool.cc   agentenv-sandbox)
agentenv_add_test(test_sandbox_fc_instance test_sandbox_fc_instance.cc agentenv-sandbox Threads::Threads)
agentenv_add_test(test_orchestrator        test_orchestrator.cc        agentenv-orchestrator)
agentenv_add_test(test_orchestrator_types  test_orchestrator_types.cc  agentenv-orchestrator)
agentenv_add_test(test_store               test_store.cc               agentenv-orchestrator)
agentenv_add_test(test_image_reference     test_image_reference.cc     agentenv-image)
agentenv_add_test(test_image_oci  test_image_oci.cc   agentenv-image)
# Rust: src/image/mod.rs (`PublishedImage`)
agentenv_add_test(test_image_published   test_image_published.cc   agentenv-image)
# Rust: src/image/catalog.rs
agentenv_add_test(test_image_catalog     test_image_catalog.cc     agentenv-image agentenv-volume-record)
# Rust: src/image/resolver.rs (free functions)
agentenv_add_test(test_image_resolver    test_image_resolver.cc    agentenv-image)
# Rust: src/types/image_tag.rs, src/repository/layout.rs, posixfs documents
agentenv_add_test(test_repository_documents test_repository_documents.cc agentenv-snapshot agentenv-volume-record)
# Rust: src/repository/backends/oss/documents.rs — the ETag compare-and-swap.
agentenv_add_test(test_oss_documents    test_oss_documents.cc    agentenv-snapshot agentenv-volume-record)
# Rust: src/repository/backends/oss/client.rs — the S3 REST calls.
agentenv_add_test(test_oss_client       test_oss_client.cc       agentenv-snapshot agentenv-volume-record agentenv-cfg)
# Rust: src/repository/backends/oss/repository.rs — documents + blobs assembled.
agentenv_add_test(test_oss_repository   test_oss_repository.cc   agentenv-snapshot agentenv-volume-record agentenv-cfg)
# Rust: src/snapshot/catalog.rs — the record layer every backend shares.
agentenv_add_test(test_record_catalog   test_record_catalog.cc   agentenv-snapshot agentenv-volume-record agentenv-cfg)
# Rust: src/repository/backends/oss/lease.rs — the append-only lease.
agentenv_add_test(test_oss_lease        test_oss_lease.cc        agentenv-snapshot agentenv-volume-record agentenv-cfg)
# Rust: src/image/local_layer.rs
agentenv_add_test(test_image_local_layer   test_image_local_layer.cc   agentenv-image)
agentenv_add_test(test_image_commit_index  test_image_commit_index.cc  agentenv-image)
agentenv_add_test(test_image_cache     test_image_cache.cc    agentenv-image)
# Rust: the `ImageCacheMetadataStore` tests in src/image/cache/graph.rs.
agentenv_add_test(test_image_cache_metadata_store test_image_cache_metadata_store.cc agentenv-image)
# Rust: the deleting-GC tests in src/image/cache/service.rs.
agentenv_add_test(test_image_cache_gc     test_image_cache_gc.cc      agentenv-image)
# Rust: the tests in src/image/cache/{service,source_config}.rs.
agentenv_add_test(test_image_cache_service test_image_cache_service.cc agentenv-image)
agentenv_add_test(test_p2p                 test_p2p.cc                 agentenv-p2p)
# Rust: the catalog tests in src/p2p/iroh/{catalog,endpoint}.rs.
agentenv_add_test(test_p2p_catalog         test_p2p_catalog.cc         agentenv-p2p)
# No Rust counterpart: the HTTP transport, an alternative to iroh for clusters
# whose peers are directly routable.
agentenv_add_test(test_p2p_http_transport  test_p2p_http_transport.cc  agentenv-p2p)
# Two transports over real loopback sockets. Needs agentenv-api for the
# server adapter, which is why this cannot live in the suite above: that one
# must keep agentenv-p2p testable on its own.
agentenv_add_test(test_p2p_http_e2e        test_p2p_http_e2e.cc        agentenv-p2p agentenv-api)
# The scheduler discovery wire. Rust uses gRPC here; this covers the HTTP
# stand-in, including against a real loopback scheduler -- hence agentenv-api.
agentenv_add_test(test_p2p_scheduler_http  test_p2p_scheduler_http.cc  agentenv-p2p agentenv-api)
agentenv_add_test(test_storage_util        test_storage_util.cc        agentenv-storage-util)
# Rust: storage/util/src/io_ring/ — the submission path ublk commands ride on
agentenv_add_test(test_io_ring             test_io_ring.cc             agentenv-storage-util)
agentenv_add_test(test_storage_overlaybd   test_storage_overlaybd.cc   agentenv-overlaybd agentenv-ublk)
# Rust: storage/ublk/src/ctrl.rs — the eight control commands + ublk ABI layout
agentenv_add_test(test_ublk_ctrl           test_ublk_ctrl.cc           agentenv-ublk)
# Rust: storage/ublk/src/{queue,io_buffer}.rs — the queue plane
agentenv_add_test(test_ublk_queue          test_ublk_queue.cc          agentenv-ublk)
# Rust: storage/ublk/src/dev.rs — build + the start sequence
agentenv_add_test(test_ublk_dev            test_ublk_dev.cc            agentenv-ublk)
# Rust: storage/ublk/src/dev.rs — queue_work/slot_task
agentenv_add_test(test_ublk_queue_worker   test_ublk_queue_worker.cc   agentenv-ublk)
# Rust: storage/ublk/src/impls/overlaybd_target.rs — the data plane
agentenv_add_test(test_ublk_overlaybd_target test_ublk_overlaybd_target.cc agentenv-ublk)
# Rust: storage/ublk-daemon/src/main.rs — flag > env > TOML, per field
agentenv_add_test(test_ublk_daemon_config  test_ublk_daemon_config.cc  agentenv-ublk-daemon-config)
# Rust: storage/ublk-daemon/src/runtime.rs — claim, upper, resize, rollback
agentenv_add_test(test_ublk_daemon_runtime test_ublk_daemon_runtime.cc agentenv-ublk-daemon-runtime)
# Rust: storage/ublk-daemon/src/server.rs — the socket half: framing, dispatch,
# error classification, and the device-handoff rollback. Links the client too,
# because the only honest test of the accept loop is a real client calling it.
agentenv_add_test(test_ublk_daemon_server  test_ublk_daemon_server.cc  agentenv-ublk-daemon-server agentenv-ublk-daemon-protocol)
# Rust: storage/ublk-daemon/src/server.rs — the device half: the device table,
# the warm pool, refcounted shared access. The cases that need a real device
# skip themselves on a kernel without ublk; what always runs is the dispatch
# and the best-effort paths, which is where a host with no ublk is either
# reported honestly or hidden.
agentenv_add_test(test_ublk_daemon_device_plane test_ublk_daemon_device_plane.cc agentenv-ublk-daemon-device-plane)
agentenv_add_test(test_lsmt                test_lsmt.cc                agentenv-overlaybd)
agentenv_add_test(test_lsmt_bptree         test_lsmt_bptree.cc         agentenv-overlaybd)
agentenv_add_test(test_scheduler           test_scheduler.cc           agentenv-scheduler-internal)
# The scheduler's P2P surface: the artifact index, the handlers, and the real
# node-side wire pointed at the real handlers over loopback -- the only
# arrangement that can catch the two halves of the protocol disagreeing.
agentenv_add_test(test_scheduler_p2p       test_scheduler_p2p.cc       agentenv-scheduler-internal agentenv-p2p)
agentenv_add_test(test_gateway             test_gateway.cc             agentenv-gateway-internal)
agentenv_add_test(test_snapshot_repository test_snapshot_repository.cc agentenv-snapshot)
agentenv_add_test(test_shell_util          test_shell_util.cc  agentenv-shell-util)
agentenv_add_test(test_warm_pool    test_warm_pool.cc           agentenv-core)
agentenv_add_test(test_api_pagination     test_api_pagination.cc      agentenv-api agentenv-snapshot)
# Rust: the framing/routing half of src/api/mod.rs + the error->status mapping
agentenv_add_test(test_api_http_server    test_api_http_server.cc     agentenv-api)
# Rust: src/api/impls/attached_drives.rs
agentenv_add_test(test_api_attached_drives test_api_attached_drives.cc agentenv-api)
# Rust: src/api/impls/auth.rs + the routing half of src/api/proxy.rs
agentenv_add_test(test_api_auth           test_api_auth.cc            agentenv-api)
# Rust: src/api/impls/admin.rs
agentenv_add_test(test_api_admin          test_api_admin.cc           agentenv-api)
# Rust: src/api/impls/sandbox.rs :: sandboxes_sandbox_id_fork_post
agentenv_add_test(test_api_sandbox_fork   test_api_sandbox_fork.cc   agentenv-api agentenv-orchestrator)
# Rust: src/api/impls/sandbox.rs :: prepare_volume_fork_specs
agentenv_add_test(test_api_volume_fork    test_api_volume_fork.cc    agentenv-api agentenv-volume)
# Rust: src/api/impls/sandbox.rs :: sandboxes_post (volume mount section)
agentenv_add_test(test_api_sandbox_volume_mounts test_api_sandbox_volume_mounts.cc
                  agentenv-api agentenv-orchestrator agentenv-volume)
# Rust: src/api/impls/volumes.rs
agentenv_add_test(test_api_volumes        test_api_volumes.cc         agentenv-api agentenv-volume)
# Rust: src/api/impls/template_helpers.rs
agentenv_add_test(test_api_template_helpers test_api_template_helpers.cc agentenv-api agentenv-template)
# Rust: src/api/impls/sandbox.rs
agentenv_add_test(test_api_sandbox_params  test_api_sandbox_params.cc  agentenv-api agentenv-sandbox)
# Rust: hyper / hyper-util legacy Client (HTTP over AF_UNIX + TCP)
agentenv_add_test(test_core_http           test_core_http.cc           agentenv-core agentenv-sandbox Threads::Threads)
agentenv_add_test(test_core_rwlock         test_core_rwlock.cc         agentenv-core Threads::Threads)
# Rust: src/sandbox/firecracker/{process_vm_reader,pool,startup_pack,config}.rs
agentenv_add_test(test_firecracker_lifecycle test_firecracker_lifecycle.cc agentenv-sandbox)
# Rust: src/sandbox/firecracker/overlaybd_snapshot.rs (dirty-page mapping)
agentenv_add_test(test_firecracker_dirty_memory test_firecracker_dirty_memory.cc agentenv-sandbox)
# Rust: src/api/impls/image_build.rs
agentenv_add_test(test_api_image_build     test_api_image_build.cc     agentenv-api)
# Rust: src/image/cache/service.rs (operation holds)
agentenv_add_test(test_image_cache_hold    test_image_cache_hold.cc    agentenv-image)
# Rust: src/image/buildkit.rs, src/image/buildkit/history.rs
agentenv_add_test(test_image_buildkit test_image_buildkit.cc agentenv-image)
agentenv_add_test(test_posixfs_layout     test_posixfs_layout.cc      agentenv-snapshot)
agentenv_add_test(test_snapshot_record    test_snapshot_record.cc     agentenv-snapshot agentenv-template)
agentenv_add_test(test_posixfs_catalog    test_posixfs_catalog.cc     agentenv-snapshot agentenv-volume-record)
agentenv_add_test(test_posixfs_artifacts  test_posixfs_artifacts.cc   agentenv-snapshot agentenv-overlaybd)
agentenv_add_test(test_artifact_cache     test_artifact_cache.cc      agentenv-snapshot)
# Rust: the `SnapshotP2pArtifact` tests in src/snapshot/p2p.rs.
agentenv_add_test(test_snapshot_p2p       test_snapshot_p2p.cc        agentenv-snapshot)
agentenv_add_test(test_image_export_target test_image_export_target.cc agentenv-snapshot)
agentenv_add_test(test_observability_host test_observability_host.cc agentenv-observability-node)
agentenv_add_test(test_observability_prometheus test_observability_prometheus.cc agentenv-observability-node)
agentenv_add_test(test_observability_reporter test_observability_reporter.cc agentenv-observability-node)
agentenv_add_test(test_orchestrator_proxy test_orchestrator_proxy.cc agentenv-orchestrator)
agentenv_add_test(test_orchestrator_lifecycle test_orchestrator_lifecycle.cc agentenv-orchestrator Threads::Threads)
agentenv_add_test(test_orchestrator_launch test_orchestrator_launch.cc agentenv-orchestrator)
agentenv_add_test(test_orchestrator_delete test_orchestrator_delete.cc agentenv-orchestrator Threads::Threads)
agentenv_add_test(test_orchestrator_fork_snapshot test_orchestrator_fork_snapshot.cc agentenv-orchestrator)
agentenv_add_test(test_orchestrator_persistence test_orchestrator_persistence.cc agentenv-orchestrator)
# Rust: src/orchestrator/sandbox_metrics.rs — the sweep over a scripted
# EnvdApi, which `SandboxBackend::Envd()` made drivable without a VMM.
agentenv_add_test(test_orchestrator_guest_metrics test_orchestrator_guest_metrics.cc agentenv-orchestrator)
agentenv_add_test(test_firecracker_factory test_firecracker_factory.cc agentenv-orchestrator)
agentenv_add_test(test_firecracker_paused_state test_firecracker_paused_state.cc agentenv-sandbox)
agentenv_add_test(test_firecracker_instance_api test_firecracker_instance_api.cc agentenv-sandbox)
agentenv_add_test(test_firecracker_configure test_firecracker_configure.cc agentenv-sandbox)
agentenv_add_test(test_firecracker_boot test_firecracker_boot.cc agentenv-sandbox)
agentenv_add_test(test_firecracker_sandbox_backend test_firecracker_sandbox_backend.cc agentenv-sandbox)
# Rust: start_fresh's host drives (tools / user rootfs / extra drives / volume slots)
agentenv_add_test(test_firecracker_fresh_drives test_firecracker_fresh_drives.cc agentenv-sandbox)
# Rust: from_snapshot_config_with_override + start_resume + build_from_paused_state
agentenv_add_test(test_firecracker_resume test_firecracker_resume.cc agentenv-sandbox)

# The boot tests spawn the stand-in VMM, so they need its path and must not be
# built before it exists.
foreach(_t test_firecracker_boot test_firecracker_sandbox_backend test_firecracker_resume)
    if(TARGET ${_t})
        add_dependencies(${_t} fake_firecracker)
        target_compile_definitions(${_t} PRIVATE
            AGENTENV_FAKE_FIRECRACKER="${AGENTENV_FAKE_FC}")
    endif()
endforeach()
# End-to-end: starts the real binary and drives it over a real socket.
#
# Everything above runs against in-process fakes. This is the only check that
# the process binds, routes and answers at all -- the gap that let the binary
# build and pass every test while `ListenAndServe` returned "not implemented".
find_program(AGENTENV_BASH bash)
if(AGENTENV_BASH AND TARGET agentenv)
    add_test(NAME smoke_api
             COMMAND ${AGENTENV_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke_api.sh
                     $<TARGET_FILE:agentenv>)
    # 77 means the runtime refused to start for want of CAP_NET_ADMIN /
    # CAP_SYS_ADMIN, which an unprivileged container cannot grant. Reported as
    # a skip so the suite stays honest about what it did and did not verify:
    # a permanent red here would be ignored within a week, and relaxing the
    # capability gate to suit the test would remove a production check.
    set_tests_properties(smoke_api PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
