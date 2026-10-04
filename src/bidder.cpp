#include "bidder.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace bidforge {
namespace {
Money integer(const Json &j, Money low, Money high) {
    if (!j.is_number_integer() ||
        (j.is_number_unsigned() && j.get<std::uint64_t>() > static_cast<std::uint64_t>(high)))
        throw std::invalid_argument("expected bounded integer");
    auto value = j.get<Money>();
    if (value < low || value > high)
        throw std::invalid_argument("integer out of range");
    return value;
}
std::string name(const Json &j) {
    if (!j.is_string())
        throw std::invalid_argument("expected string");
    auto s = j.get<std::string>();
    if (s.empty() || s.size() > 128)
        throw std::invalid_argument("string length must be 1..128 bytes");
    return s;
}
std::vector<std::string> categories(const Json &j) {
    if (!j.is_array() || j.empty() || j.size() > 64)
        throw std::invalid_argument("categories must contain 1..64 strings");
    std::vector<std::string> result;
    for (const auto &item : j)
        result.push_back(name(item));
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
bool eligible(const Campaign &c, const Request &r) {
    if (!c.active || c.width != r.width || c.height != r.height || c.bid < r.floor)
        return false;
    for (const auto &cat : r.categories)
        if (std::binary_search(c.categories.begin(), c.categories.end(), cat))
            return true;
    return false;
}
Reply error(int status, const char *message) {
    return {status, {{"error", message}}};
}
} // namespace
Money floor_milli_cpm(const Json &j) {
    if (!j.is_number())
        throw std::invalid_argument("bidfloor must be a number");
    const double price = j.get<double>();
    if (!std::isfinite(price) || price < 0 || price > 1000000)
        throw std::invalid_argument("bidfloor must be 0..1000000 USD CPM");
    const double scaled = price * 1000;
    const double rounded = std::round(scaled);
    // Allow only the small error caused by representing a decimal in binary.
    const double tolerance = 4 * std::numeric_limits<double>::epsilon() * std::max(1.0, scaled);
    if (std::abs(scaled - rounded) > tolerance)
        throw std::invalid_argument("bidfloor supports at most three decimal places");
    return static_cast<Money>(rounded);
}
std::vector<Campaign> parse_campaigns(const Json &j) {
    if (!j.is_array() || j.empty())
        throw std::invalid_argument("campaign file must be a nonempty array");
    std::vector<Campaign> result;
    std::set<std::string> ids;
    for (const auto &v : j) {
        Campaign c;
        c.id = name(v.at("id"));
        c.creative = name(v.at("creative_id"));
        if (!v.at("active").is_boolean())
            throw std::invalid_argument("active must be boolean");
        c.active = v.at("active").get<bool>();
        c.categories = categories(v.at("categories"));
        c.width = static_cast<int>(integer(v.at("width"), 1, 10000));
        c.height = static_cast<int>(integer(v.at("height"), 1, 10000));
        c.bid = integer(v.at("bid_milli_cpm"), 1, max_bid);
        c.budget = integer(v.at("budget_micros"), 0, std::numeric_limits<Money>::max());
        if (c.bid > c.budget)
            throw std::invalid_argument("budget must cover one bid");
        if (!ids.insert(c.id).second)
            throw std::invalid_argument("duplicate campaign ID");
        result.push_back(std::move(c));
    }
    return result;
}
Request parse_request(const Json &j) {
    Request r;
    r.id = name(j.at("id"));
    r.tmax = static_cast<int>(integer(j.at("tmax"), 1, 60000));
    const auto &impressions = j.at("imp");
    if (!impressions.is_array() || impressions.size() != 1)
        throw std::invalid_argument("exactly one impression required");
    const auto &imp = impressions.at(0);
    if (imp.contains("video") || imp.contains("native") || imp.contains("audio"))
        throw std::invalid_argument("banner only");
    r.impression = name(imp.at("id"));
    r.width = static_cast<int>(integer(imp.at("banner").at("w"), 1, 10000));
    r.height = static_cast<int>(integer(imp.at("banner").at("h"), 1, 10000));
    if (imp.contains("bidfloorcur") && imp.at("bidfloorcur") != "USD")
        throw std::invalid_argument("USD only");
    if (j.contains("cur") && j.at("cur") != Json::array({"USD"}))
        throw std::invalid_argument("USD only");
    if (j.contains("at") && integer(j.at("at"), 1, 1) != 1)
        throw std::invalid_argument("first price only");
    r.floor = floor_milli_cpm(imp.value("bidfloor", Json(0)));
    r.categories = categories(j.at("site").at("cat"));
    return r;
}
bool Request::operator==(const Request &o) const {
    return std::tie(id, impression, tmax, width, height, floor, categories) ==
           std::tie(o.id, o.impression, o.tmax, o.width, o.height, o.floor, o.categories);
}
} // namespace bidforge
