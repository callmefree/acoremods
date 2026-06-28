// Slice 8b cleanup — concurrency contract test for the atomic-
// shared_ptr publish/load pattern that replaces `_mutex` across the
// Mgr (`_combatHot`, `_rotationSnap`, `_eventsSnap`,
// `_eventBossSpawnSnap`).
//
// The Mgr singleton itself can't be stood up in a unit harness (it
// pulls in WorldDatabase, sWorldSessionMgr, sObjectMgr, etc.), so
// this test exercises a faithful copy of the publish/load shape on
// a stand-in payload type. If a future change regresses the memory
// ordering, drops the shared_ptr in favor of a raw pointer, or
// accidentally lets a writer mutate a published snapshot in place,
// this test will fire under TSan / ASan — or just produce a torn
// read on plain x86_64 + libstdc++ 11.
//
// Sized small (3 threads, ~50 ms) so it stays in the regular
// `unit_tests` pass without slowing the suite.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
    // Stand-in payload — same shape as `CombatHotState::slots`: a
    // small POD vector that readers iterate. Each writer publish
    // bumps `serial` and fills the vector with `serial` repeated, so
    // a reader can verify "every entry is the same value" as a
    // proof-of-no-torn-read invariant.
    struct Payload
    {
        std::uint64_t serial;
        std::vector<std::uint64_t> entries;
    };

    // Writer-thread-private mutable state mirrors the Mgr pattern
    // (writer mutates directly, then atomic-store publishes a fresh
    // shared_ptr<const>). One writer thread for this test (the live
    // Mgr also has exactly one writer thread — the world thread).
    std::shared_ptr<Payload const> MakePayload(std::uint64_t serial,
                                                std::size_t count)
    {
        auto p = std::make_shared<Payload>();
        p->serial = serial;
        p->entries.assign(count, serial);
        return p;
    }
}

TEST(SnapshotConcurrency, NoTornReadsAcrossPublishes)
{
    std::shared_ptr<Payload const> snap = MakePayload(0, 8);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> readerIters{0};
    std::atomic<std::uint64_t> writerIters{0};
    std::atomic<bool> readerSawTearing{false};

    auto reader = [&]
    {
        while (!stop.load(std::memory_order_relaxed))
        {
            auto p = std::atomic_load_explicit(
                &snap, std::memory_order_acquire);
            if (!p)
                continue;
            std::uint64_t expected = p->serial;
            for (std::uint64_t v : p->entries)
            {
                if (v != expected)
                {
                    readerSawTearing.store(true,
                        std::memory_order_relaxed);
                    return;
                }
            }
            readerIters.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread r1(reader);
    std::thread r2(reader);

    std::thread w([&]
    {
        std::uint64_t serial = 1;
        while (!stop.load(std::memory_order_relaxed))
        {
            auto fresh = MakePayload(serial, 8 + (serial & 0x3));
            std::atomic_store_explicit(&snap, fresh,
                                        std::memory_order_release);
            ++serial;
            writerIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();
    w.join();

    EXPECT_FALSE(readerSawTearing.load(std::memory_order_relaxed))
        << "reader observed a torn snapshot — atomic publish/load "
           "contract is broken";
    // Sanity: both threads ran enough iterations that a torn read
    // would have surfaced if the contract were broken.
    EXPECT_GT(readerIters.load(std::memory_order_relaxed), 100u);
    EXPECT_GT(writerIters.load(std::memory_order_relaxed), 100u);
}

TEST(SnapshotConcurrency, NoUseAfterFreeOnReplace)
{
    // Reader can hold the shared_ptr past a writer replacement. The
    // writer's atomic-store drops only the writer's own reference;
    // the reader's strong reference keeps the old payload alive
    // until the reader releases it. This is the property that lets
    // hot-path readers skip locking entirely.
    auto first  = MakePayload(11, 4);
    std::shared_ptr<Payload const> snap = first;

    auto held = std::atomic_load_explicit(&snap,
                                           std::memory_order_acquire);
    ASSERT_EQ(held->serial, 11u);

    auto second = MakePayload(22, 4);
    std::atomic_store_explicit(&snap, second,
                                std::memory_order_release);

    // The reader's `held` still points at the first payload — the
    // writer's replacement didn't free it out from under us.
    EXPECT_EQ(held->serial, 11u);
    EXPECT_EQ(held->entries.size(), 4u);
    for (std::uint64_t v : held->entries)
        EXPECT_EQ(v, 11u);

    // A fresh reader sees the new payload.
    auto fresh = std::atomic_load_explicit(&snap,
                                            std::memory_order_acquire);
    EXPECT_EQ(fresh->serial, 22u);
}
