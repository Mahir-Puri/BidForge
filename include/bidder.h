#pragma once
#include "json.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bidforge {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Money = std::int64_t;
constexpr Money max_bid = 1000000000;
struct Campaign {
    std::string id, creative;
    bool active;
    std::vector<std::string> categories;
    int width, height;
    Money bid, budget;
};
struct Request {
    std::string id, impression;
    int tmax, width, height;
    Money floor;
    std::vector<std::string> categories;
    bool operator==(const Request &other) const;
};
std::vector<Campaign> parse_campaigns(const Json &value);
Request parse_request(const Json &value);
Money floor_milli_cpm(const Json &value);
struct Reply {
    int status;
    Json body;
};
struct Account {
    Money committed = 0, reserved = 0;
};
struct Record {
    Request request;
    Reply reply;
    std::size_t campaign = 0;
    std::string state = "none";
};
// Own the mutex with the state it protects; campaigns and the index never change.
class Bidder {
  public:
    Bidder(std::vector<Campaign> campaigns, bool indexed, std::size_t capacity);
    std::vector<std::size_t> match(const Request &request) const;
    Reply bid(const Request &request, Clock::time_point start);
    Reply outcome(const std::string &bid_id, const std::string &result);
    Json stats() const;
    void validation_failure();

  private:
    const std::vector<Campaign> campaigns_;
    const bool indexed_;
    const std::size_t capacity_;
    std::unordered_map<std::string, std::vector<std::size_t>> index_;
    mutable std::mutex mutex_;
    std::vector<Account> accounts_;
    std::unordered_map<std::string, Record> records_;
    std::unordered_map<std::string, std::string> bid_requests_;
    std::uint64_t received_ = 0, valid_ = 0, duplicates_ = 0, bids_ = 0;
    std::uint64_t no_bids_ = 0, invalid_ = 0, deadlines_ = 0, overload_ = 0;
    std::uint64_t matching_ns_ = 0, matching_calls_ = 0;
};
} // namespace bidforge
