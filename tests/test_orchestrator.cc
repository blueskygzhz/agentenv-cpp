// SPDX-License-Identifier: MIT
#include "microtest.h"

#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/sandbox/mock.h"

using namespace agentenv;

MT_TEST(orchestrator_end_to_end) {
    auto backend   = std::shared_ptr<sandbox::Backend>(new sandbox::MockBackend());
    auto persister = std::shared_ptr<orchestrator::Persister>(
                        orchestrator::MakePersister("memory", "").release());
    orchestrator::Service svc(backend, persister);

    orchestrator::CreateOptions opts;
    opts.template_id = "python:3.11";
    auto fut = svc.Create(opts);
    auto res = fut.get();
    MT_EXPECT_TRUE(res.ok());
    auto sb = res.value();
    MT_EXPECT_TRUE(sb.phase == orchestrator::LifecyclePhase::Running);

    auto list = svc.List();
    MT_EXPECT_TRUE(list.ok());
    MT_EXPECT_EQ(static_cast<int>(list.value().size()), 1);

    auto sfut = svc.Stop(sb.id);
    MT_EXPECT_TRUE(sfut.get().ok());

    auto after = svc.Get(sb.id).value();
    MT_EXPECT_TRUE(after.phase == orchestrator::LifecyclePhase::Stopped);
}

MT_MAIN
