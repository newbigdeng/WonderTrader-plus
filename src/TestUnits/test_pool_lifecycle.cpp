#include "gtest/gtest/gtest.h"
#include "../Includes/WTSTradeDef.hpp"
#include <atomic>
#include <thread>
#include <vector>

namespace
{
class PoolProbe : public wtp::WTSPoolObject<PoolProbe>
{
public:
    std::atomic<int>* destroyed = nullptr;
    int value = 0;
    ~PoolProbe() { if (destroyed) ++*destroyed; }
};
}

TEST(PoolLifecycle, RetainedOrderSurvivesAllocatingThreadExit)
{
    wtp::WTSOrderInfo* order = nullptr;
    std::thread producer([&] {
        order = wtp::WTSOrderInfo::create();
        order->setVolume(3);
        order->setVolTraded(1);
        order->setVolLeft(2);
        order->setOrderDate(20261002);
        order->retain();
        order->release();
    });
    producer.join();
    ASSERT_NE(order, nullptr);
    EXPECT_DOUBLE_EQ(order->getVolume(), 3);
    EXPECT_DOUBLE_EQ(order->getVolTraded(), 1);
    EXPECT_DOUBLE_EQ(order->getVolLeft(), 2);
    EXPECT_EQ(order->getOrderDate(), 20261002u);
    order->release();
}

TEST(PoolLifecycle, LastReferenceReleasedOnDifferentThread)
{
    std::atomic<int> destroyed{0};
    std::vector<PoolProbe*> objects;
    std::thread producer([&] {
        for (int i = 0; i < 512; ++i)
        {
            auto object = PoolProbe::allocate();
            object->value = i;
            object->destroyed = &destroyed;
            objects.push_back(object);
        }
    });
    producer.join();
    std::thread consumer([&] {
        for (int i = 0; i < 512; ++i)
        {
            EXPECT_EQ(objects[i]->value, i);
            objects[i]->release();
        }
    });
    consumer.join();
    EXPECT_EQ(destroyed, 512);
}

TEST(PoolLifecycle, ConcurrentFinalReleasesAfterProducerExit)
{
    std::atomic<int> destroyed{0};
    std::vector<PoolProbe*> objects;
    std::thread producer([&] {
        for (int i = 0; i < 512; ++i)
        {
            auto object = PoolProbe::allocate();
            object->destroyed = &destroyed;
            objects.push_back(object);
        }
    });
    producer.join();
    std::vector<std::thread> consumers;
    for (int thread = 0; thread < 8; ++thread)
        consumers.emplace_back([&, thread] {
            for (int i = thread; i < 512; i += 8)
                objects[i]->release();
        });
    for (auto& consumer : consumers)
        consumer.join();
    EXPECT_EQ(destroyed, 512);
}
