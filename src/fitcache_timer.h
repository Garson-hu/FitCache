#pragma once
#include <chrono>
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <cstring>

namespace fitcache {

struct Stat {
    std::atomic<double> total_us{0.0};     // 总耗时（微秒）
    std::atomic<uint64_t> calls{0};        // 调用次数

    // Default constructor for unordered_map when key is not found
    Stat() = default;
};

inline std::unordered_map<std::string, Stat>& get_table() {
    static std::unordered_map<std::string, Stat> tbl;
    return tbl;
}

inline std::mutex& get_mutex() {
    static std::mutex m;
    return m;
}

class TimerGuard {
public:
    explicit TimerGuard(const char* tag)
        : tag_(tag), start_(std::chrono::steady_clock::now()) {}

    ~TimerGuard() {
        // Use std::chrono::steady_clock consistently
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();

        auto& tbl = get_table();
        {
            std::lock_guard<std::mutex> lk(get_mutex());
            auto& s = tbl[tag_]; // This might create a new Stat if tag_ doesn't exist

            // Atomically add to total_us for double
            double current_total = s.total_us.load(std::memory_order_relaxed);
            double new_total;
            do {
                new_total = current_total + static_cast<double>(us);
            } while (!s.total_us.compare_exchange_weak(current_total, new_total,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed));

            s.calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
private:
    const char* tag_;
    std::chrono::steady_clock::time_point start_;
};

inline void print_all_stats() {
    std::stringstream ss;

    const int section_col_width = 45;
    const int calls_col_width = 12;
    const int total_us_col_width = 18;
    const int avg_us_col_width = 15;

    const int total_line_width = section_col_width + calls_col_width + total_us_col_width + avg_us_col_width;

    ss << "\n=== FitCache timing summary ===\n"
       << std::left << std::setw(section_col_width) << "Section"
       << std::right << std::setw(calls_col_width) << "Calls"
       << std::right << std::setw(total_us_col_width) << "Total(us)"
       << std::right << std::setw(avg_us_col_width) << "Avg(us)"
       << "\n" << std::string(total_line_width, '-') << "\n";

    {
        std::lock_guard<std::mutex> lk(get_mutex());
        for (auto const& [k, v] : get_table()) {
            double tot = v.total_us.load(std::memory_order_acquire);
            uint64_t c = v.calls.load(std::memory_order_acquire);
            ss << std::left  << std::setw(section_col_width) << k
               << std::right << std::setw(calls_col_width) << c
               << std::right << std::setw(total_us_col_width) << std::fixed << std::setprecision(2) << tot
               << std::right << std::setw(avg_us_col_width) << std::fixed << std::setprecision(2) << (c ? tot/static_cast<double>(c) : 0.0)
               << "\n";
        }
    }
    ss << std::string(total_line_width, '=') << "\n";
    std::cout << ss.str();
}

inline void reset_all_stats() {
    std::lock_guard<std::mutex> lk(get_mutex());
    auto& tbl = get_table();
    for (auto& pair_in_map : tbl) {
        pair_in_map.second.total_us.store(0.0, std::memory_order_relaxed);
        pair_in_map.second.calls.store(0, std::memory_order_relaxed);
    }
    std::cout << "! FitCache Timing Stats have been RESET  !" << std::endl;
}

inline void export_all_stats_to_file(const char* filename) {
    if (!filename || strlen(filename) == 0) {
        std::cerr << "Error: export_all_stats_to_file called with invalid filename. Printing to console instead." << std::endl;
        print_all_stats(); // Fallback to printing to console
        return;
    }

    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        std::cerr << "Error: Could not open file '" << filename << "' for writing stats. Printing to console instead." << std::endl;
        print_all_stats(); // Fallback
        return;
    }

    outfile << std::left << std::setw(45) << "Section"
            << std::right << std::setw(12) << "Calls"
            << std::setw(15) << "Total(us)"
            << std::setw(15) << "Avg(us)"
            << "\n-----------------------------------------------------------------------------------\n";

    {
        std::lock_guard<std::mutex> lk(get_mutex());
        for (auto const& [k, v] : get_table()) {
            double tot = v.total_us.load(std::memory_order_acquire);
            uint64_t c = v.calls.load(std::memory_order_acquire);
            outfile << std::left << std::setw(45) << k
                    << std::right << std::setw(12) << c
                    << std::setw(15) << std::fixed << std::setprecision(2) << tot
                    << std::setw(15) << (c ? tot / static_cast<double>(c) : 0.0)
                    << "\n";
        }
    }
    outfile << "===================================================================================\n";
    outfile.close();
    std::cout << "[FitCache Server] Timing summary exported to " << filename << std::endl;
}

} // namespace fitcache

#define FitCache_TIMING(name) fitcache::TimerGuard __fitcache_tg__(name) 