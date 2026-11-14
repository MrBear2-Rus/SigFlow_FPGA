// K0 feasibility spike, NOT the production VirtualSession/K2 implementation.
// One process and one Verilated model; only the worker owns/evaluates the DUT.
#include "Vk0_counter.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>

using Json = nlohmann::json;

// Required by the bundled legacy runtime; explicit VerilatedContext::time()
// remains the integer simulation clock used by this spike.
double sc_time_stamp() { return 0.0; }

class RationalClock {
public:
    std::uint64_t next() {
        // floor and remainder accumulation of n * 1e12 / 54e6, nearest ties up.
        constexpr std::uint64_t denominator = 54000000;
        constexpr std::uint64_t quotient = 1000000000000ULL / denominator;
        constexpr std::uint64_t residue = 1000000000000ULL % denominator;
        const auto nextRemainder = remainder_ + residue;
        const auto advance = quotient + nextRemainder / denominator;
        if (floor_ > std::numeric_limits<std::uint64_t>::max() - advance - 1)
            throw std::overflow_error("clock tick overflow");
        floor_ += advance;
        remainder_ = nextRemainder % denominator;
        ++edges_;
        return floor_ + (remainder_ >= denominator / 2 ? 1 : 0);
    }
    std::uint64_t edges() const { return edges_; }
private:
    std::uint64_t floor_ = 0, remainder_ = 0, edges_ = 0;
};

struct Request {
    Json command;
    std::promise<Json> result;
};

class Spike {
public:
    explicit Spike(std::string wavePath) : wavePath_(std::move(wavePath)) {
        auto ready = ready_.get_future();
        worker_ = std::thread([this] { run(); });
        try { ready.get(); }
        catch (...) { worker_.join(); throw; }
    }
    ~Spike() {
        try { if (!finished_) submit({{"op", "stop"}}); } catch (...) {}
        if (worker_.joinable()) worker_.join();
    }
    Json submit(Json command) {
        auto request = std::make_shared<Request>();
        request->command = std::move(command);
        auto result = request->result.get_future();
        {
            std::lock_guard lock(mutex_);
            if (finished_) throw std::runtime_error("runner stopped");
            queue_.push_back(request);
        }
        condition_.notify_one();
        return result.get();
    }
private:
    void run() {
        try {
            VerilatedContext context;
            context.threads(1);
            context.traceEverOn(true);
            Vk0_counter dut{&context};
            VerilatedVcdC wave;
            dut.trace(&wave, 3);
            wave.open(wavePath_.c_str());
            std::uint64_t tick = 0, evaluations = 0, writes = 0;
            unsigned finalCalls = 0;
            RationalClock clock;
            std::string state = "paused";
            std::map<std::string, bool> pending;
            dut.clock_in = 0; dut.reset_n = 0; dut.gate_in = 0;
            context.time(0); dut.eval(); ++evaluations; wave.dump(0);
            const auto snapshot = [&] {
                return Json{{"state", state}, {"tick_ps", "1"}, {"tick", std::to_string(tick)},
                            {"edges", std::to_string(clock.edges())}, {"count", unsigned(dut.observed)},
                            {"eval_calls", std::to_string(evaluations)}, {"input_writes", std::to_string(writes)},
                            {"pending_count", pending.size()}, {"model_instances", 1}, {"final_calls", finalCalls}};
            };
            const auto apply = [&] {
                if (pending.empty()) return;
                for (const auto& [port, value] : pending) {
                    if (port == "reset_n") dut.reset_n = value;
                    else if (port == "gate_in") dut.gate_in = value;
                    ++writes;
                }
                pending.clear();
                // A separate evaluation before clock changes settles data paths.
                dut.eval(); ++evaluations;
            };
            const auto nextRising = [&] {
                do {
                    tick = clock.next();
                    context.time(tick);
                    dut.clock_in = !dut.clock_in;
                    dut.eval(); ++evaluations;
                    wave.dump(tick);
                } while (!dut.clock_in);
            };
            ready_.set_value();
            while (state != "stopped") {
                std::shared_ptr<Request> request;
                {
                    std::unique_lock lock(mutex_);
                    if (queue_.empty() && state != "running")
                        condition_.wait(lock, [&] { return !queue_.empty(); });
                    if (!queue_.empty()) { request = queue_.front(); queue_.pop_front(); }
                }
                if (!request) { nextRising(); continue; }
                try {
                    const auto op = request->command.at("op").get<std::string>();
                    std::string status = "applied";
                    if (op == "set") {
                        const auto port = request->command.at("port").get<std::string>();
                        if (port != "reset_n" && port != "gate_in") throw std::runtime_error("unknown fixture port");
                        if (!request->command.at("value").is_boolean()) throw std::runtime_error("value must be bool");
                        pending[port] = request->command.at("value").get<bool>();
                        status = "pending";
                        if (state == "running") { apply(); status = "applied"; }
                    } else if (op == "step") {
                        if (state != "paused") throw std::runtime_error("pause before stepping");
                        const auto cycles = request->command.value("cycles", 1);
                        if (cycles < 1 || cycles > 100000) throw std::runtime_error("cycles out of range");
                        state = "stepping"; apply();
                        for (int i = 0; i < cycles; ++i) nextRising();
                        state = "paused";
                    } else if (op == "play") {
                        state = "running"; apply();
                    } else if (op == "pause") {
                        state = "paused";
                    } else if (op == "stop") {
                        state = "stopped"; pending.clear();
                        dut.final(); ++finalCalls; wave.close();
                        finished_ = true;
                    } else if (op != "get") throw std::runtime_error("unknown operation");
                    auto result = snapshot();
                    result["status"] = status;
                    request->result.set_value(std::move(result));
                } catch (const std::exception& error) {
                    auto result = snapshot();
                    result["status"] = "rejected"; result["error"] = error.what();
                    request->result.set_value(std::move(result));
                }
            }
        } catch (...) {
            finished_ = true;
            try { ready_.set_exception(std::current_exception()); } catch (...) {}
            // A fatal worker error must resolve every queued command, never hang the host.
            std::lock_guard lock(mutex_);
            for (auto& request : queue_) request->result.set_exception(std::current_exception());
        }
    }
    std::string wavePath_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<Request>> queue_;
    std::promise<void> ready_;
    std::thread worker_;
    std::atomic<bool> finished_{false};
};

int main(int argc, char** argv) {
    try {
        Spike spike(argc > 1 ? argv[1] : "interactive.vcd");
        for (std::string line; std::getline(std::cin, line);) {
            try {
                auto response = spike.submit(Json::parse(line));
                std::cout << response.dump() << std::endl;
                if (response.at("state") == "stopped") break;
            } catch (const std::exception& error) {
                std::cout << Json{{"status", "rejected"}, {"error", error.what()}}.dump() << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
