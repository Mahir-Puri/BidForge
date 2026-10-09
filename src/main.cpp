#include "bidder.h"
#include "httplib.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace bidforge;
namespace {
std::size_t positive(const std::string &s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected positive integer option");
    auto n = std::stoull(s);
    if (n == 0 || n > 100000000)
        throw std::invalid_argument("option out of range");
    return static_cast<std::size_t>(n);
}
void send(httplib::Response &response, const Reply &reply) {
    response.status = reply.status;
    if (reply.status != 204)
        response.set_content(reply.body.dump(), "application/json");
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::string file = "examples/campaigns.json", mode = "indexed";
        std::size_t port = 8080, workers = 8, queue = 128, capacity = 100000;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::invalid_argument("options require values");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--campaigns")
                file = value;
            else if (key == "--mode")
                mode = value;
            else if (key == "--port")
                port = positive(value);
            else if (key == "--workers")
                workers = positive(value);
            else if (key == "--queue")
                queue = positive(value);
            else if (key == "--history")
                capacity = positive(value);
            else
                throw std::invalid_argument("unknown option: " + key);
        }
        if ((mode != "baseline" && mode != "indexed") || port > 65535 || workers > 256)
            throw std::invalid_argument("invalid mode, port, or workers (maximum 256)");
        std::ifstream input(file);
        if (!input)
            throw std::runtime_error("cannot open campaigns: " + file);
        Json config;
        input >> config;
        Bidder bidder(parse_campaigns(config), mode == "indexed", capacity);
        httplib::Server server;
        server.new_task_queue = [=] { return new httplib::ThreadPool(workers, queue); };
        server.set_payload_max_length(64 * 1024);
        server.set_read_timeout(2);
        server.set_write_timeout(2);
        server.set_keep_alive_timeout(1);
        server.set_keep_alive_max_count(100);
        std::atomic<std::uint64_t> processing_ns{0}, handler_calls{0};
        server.Get("/health", [](const auto &, auto &response) {
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });
        server.Get("/stats", [&](const auto &, auto &response) {
            auto stats = bidder.stats();
            stats["handler_processing_ns"] = processing_ns.load();
            stats["handler_calls"] = handler_calls.load();
            response.set_content(stats.dump(), "application/json");
        });
        server.Post("/bid", [&](const auto &request, auto &response) {
            const auto start = Clock::now();
            Request normalized;
            try {
                normalized = parse_request(Json::parse(request.body));
            } catch (const std::exception &e) {
                bidder.validation_failure();
                send(response, {400, {{"error", e.what()}}});
                return;
            }
            send(response, bidder.bid(normalized, start));
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            processing_ns.fetch_add(static_cast<std::uint64_t>(ns));
            ++handler_calls;
            response.set_header("X-Processing-Micros", std::to_string(ns / 1000));
        });
        server.Post("/outcome", [&](const auto &request, auto &response) {
            try {
                const auto j = Json::parse(request.body);
                send(response, bidder.outcome(j.at("bid_id").template get<std::string>(),
                                              j.at("outcome").template get<std::string>()));
            } catch (const std::exception &e) {
                send(response, {400, {{"error", e.what()}}});
            }
        });
        std::cout << "BidForge listening on 127.0.0.1:" << port << " (" << mode << ")" << std::endl;
        if (!server.listen("127.0.0.1", static_cast<int>(port)))
            throw std::runtime_error("listen failed");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
