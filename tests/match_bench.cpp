#include "bidder.h"
#include <fstream>
#include <iostream>
using namespace bidforge;
int main(int argc, char **argv) {
    try {
        if (argc != 4)
            throw std::runtime_error("usage: match_bench campaigns.json category-count iterations");
        std::ifstream file(argv[1]);
        Json json;
        file >> json;
        const int categories = std::stoi(argv[2]), iterations = std::stoi(argv[3]);
        if (categories < 1 || iterations < 1)
            throw std::runtime_error("positive counts required");
        auto campaigns = parse_campaigns(json);
        Bidder baseline(campaigns, false, 1), indexed(campaigns, true, 1);
        std::vector<Request> inputs;
        for (int i = 0; i < categories; ++i)
            inputs.push_back({"r", "i", 1000, 300, 250, 0, {"cat-" + std::to_string(i)}});
        for (const auto &r : inputs)
            if (baseline.match(r) != indexed.match(r))
                throw std::runtime_error("matching mismatch");
        for (const auto *b : {&baseline, &indexed}) {
            for (int i = 0; i < 100; ++i)
                b->match(inputs[static_cast<std::size_t>(i) % inputs.size()]);
            for (int repeat = 0; repeat < 5; ++repeat) {
                std::size_t matches = 0;
                auto start = Clock::now();
                for (int i = 0; i < iterations; ++i)
                    matches += b->match(inputs[static_cast<std::size_t>(i) % inputs.size()]).size();
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
                              .count();
                std::cout << Json({{"mode", b == &baseline ? "baseline" : "indexed"},
                                   {"campaigns", campaigns.size()},
                                   {"categories", categories},
                                   {"iterations", iterations},
                                   {"repeat", repeat},
                                   {"ns_per_match", static_cast<double>(ns) / iterations},
                                   {"matches", matches}})
                                 .dump()
                          << '\n';
            }
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
