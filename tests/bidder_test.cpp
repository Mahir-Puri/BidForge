#include "bidder.h"
#include <condition_variable>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
using namespace bidforge;
namespace {
Json campaign() {
    return {{"id", "a"},
            {"creative_id", "creative"},
            {"active", true},
            {"categories", {"sport"}},
            {"width", 300},
            {"height", 250},
            {"bid_milli_cpm", 3000},
            {"budget_micros", 30000}};
}
Json request() {
    return {{"id", "r"},
            {"tmax", 1000},
            {"imp", Json::array({{{"id", "i"}, {"banner", {{"w", 300}, {"h", 250}}}}})},
            {"site", {{"cat", {"sport"}}}}};
}
std::vector<Campaign> config(Json c = campaign()) {
    return parse_campaigns(Json::array({c}));
}
Reply bid(Bidder &b, const std::string &id) {
    auto r = parse_request(request());
    r.id = id;
    return b.bid(r, Clock::now());
}
// All threads reach the gate before any of them issue a bid.
void concurrent(int count, const std::function<void(int)> &fn) {
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i)
        threads.emplace_back([&, i] {
            {
                std::unique_lock<std::mutex> lock(m);
                ++arrived;
                cv.notify_all();
                cv.wait(lock, [&] { return arrived == count; });
            }
            fn(i);
        });
    for (auto &t : threads)
        t.join();
}
} // namespace
TEST(Validation, Campaigns) {
    for (auto key : {"id", "creative_id", "active", "categories", "width", "height",
                     "bid_milli_cpm", "budget_micros"}) {
        auto c = campaign();
        c.erase(key);
        EXPECT_THROW(config(c), std::exception);
    }
    for (auto key : {"width", "height", "bid_milli_cpm"}) {
        auto c = campaign();
        c[key] = 0;
        EXPECT_THROW(config(c), std::exception);
        c[key] = -1;
        EXPECT_THROW(config(c), std::exception);
        c[key] = 1.5;
        EXPECT_THROW(config(c), std::exception);
    }
    auto c = campaign();
    c["budget_micros"] = -1;
    EXPECT_THROW(config(c), std::exception);
    c["budget_micros"] = 2999;
    EXPECT_THROW(config(c), std::exception);
    c["budget_micros"] = std::numeric_limits<std::uint64_t>::max();
    EXPECT_THROW(config(c), std::exception);
    c["budget_micros"] = std::numeric_limits<Money>::max();
    EXPECT_NO_THROW(config(c));
    c["bid_milli_cpm"] = max_bid + 1;
    EXPECT_THROW(config(c), std::exception);
    EXPECT_THROW(parse_campaigns(Json::array({campaign(), campaign()})), std::exception);
}
TEST(Validation, RequestAndFloorBoundaries) {
    EXPECT_EQ(floor_milli_cpm(2.501), 2501);
    EXPECT_EQ(floor_milli_cpm(0.001), 1);
    EXPECT_EQ(floor_milli_cpm(0.0), 0);
    EXPECT_EQ(floor_milli_cpm(1000000), max_bid);
    for (double price : {-0.001, 0.0001, 2.5001, 2.4999, 1000000.001})
        EXPECT_THROW(floor_milli_cpm(price), std::exception);
    EXPECT_THROW(floor_milli_cpm("3"), std::exception);
    auto r = request();
    r["imp"][0]["bidfloorcur"] = "EUR";
    EXPECT_THROW(parse_request(r), std::exception);
    r = request();
    r["tmax"] = 0;
    EXPECT_THROW(parse_request(r), std::exception);
    r = request();
    r["imp"].push_back(r["imp"][0]);
    EXPECT_THROW(parse_request(r), std::exception);
}
TEST(Matching, EligibilityAndTieBreak) {
    auto a = campaign(), b = a, c = a;
    b["id"] = "b";
    c["id"] = "c";
    c["bid_milli_cpm"] = 4000;
    for (bool indexed : {false, true}) {
        Bidder bidder(parse_campaigns(Json::array({b, c, a})), indexed, 100);
        auto r = parse_request(request());
        EXPECT_EQ(bidder.match(r), (std::vector<std::size_t>{1, 2, 0}));
        r.floor = 3001;
        EXPECT_EQ(bidder.match(r), (std::vector<std::size_t>{1}));
        r.floor = 4001;
        EXPECT_TRUE(bidder.match(r).empty());
        r.floor = 0;
        r.width = 728;
        EXPECT_TRUE(bidder.match(r).empty());
        r.width = 300;
        r.categories = {"unknown"};
        EXPECT_TRUE(bidder.match(r).empty());
    }
    a["active"] = false;
    Bidder inactive(config(a), true, 1);
    EXPECT_EQ(bid(inactive, "r").status, 204);
}
TEST(Matching, EquivalentAcrossManyInputs) {
    Json cs = Json::array();
    for (int i = 0; i < 100; ++i) {
        auto c = campaign();
        c["id"] = std::to_string(i);
        c["categories"] = {std::to_string(i % 7)};
        c["bid_milli_cpm"] = 1000 + i % 3;
        c["budget_micros"] = 3000;
        cs.push_back(c);
    }
    Bidder baseline(parse_campaigns(cs), false, 1000), indexed(parse_campaigns(cs), true, 1000);
    auto multi = parse_request(request());
    multi.categories = {"0", "1", "1"};
    EXPECT_EQ(baseline.match(multi), indexed.match(multi));
    for (int i = 0; i < 500; ++i) {
        auto r = parse_request(request());
        r.id = std::to_string(i);
        r.categories = {std::to_string(i % 9)};
        EXPECT_EQ(baseline.match(r), indexed.match(r));
        EXPECT_EQ(baseline.bid(r, Clock::now()).body, indexed.bid(r, Clock::now()).body);
    }
}
