#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mooncake::kvtest {

struct MetricSnapshot {
    std::string name;
    uint64_t total{0};
    uint64_t success{0};
    uint64_t failure{0};
    uint64_t bytes{0};
};

class MetricsCollector {
   public:
    MetricsCollector(int instance_id, int interval_ms, std::string output_dir);
    ~MetricsCollector();

    void Start();
    void Reset();
    void Freeze();
    void Stop();
    void Record(const std::string &operation, double latency_ms, bool success,
                uint64_t bytes);
    void RecordVerifyFailure();
    std::vector<MetricSnapshot> SnapshotCounts() const;
    std::string StatsJson() const;
    void WriteSummary();
    double ElapsedSeconds() const;

   private:
    struct RequestSample {
        int64_t elapsed_us{0};
        double latency_ms{0};
        bool success{false};
        uint64_t bytes{0};
    };

    struct OperationMetrics {
        std::atomic<uint64_t> total{0};
        std::atomic<uint64_t> success{0};
        std::atomic<uint64_t> failure{0};
        std::atomic<uint64_t> bytes{0};
        mutable std::mutex latency_mutex;
        std::vector<double> window_latencies;
        // One in-memory row per operation. The request path never writes a
        // file; Stop() emits the complete sample set in one batch.
        std::vector<RequestSample> samples;
        uint64_t flushed_success{0};
        uint64_t flushed_failure{0};
        uint64_t flushed_bytes{0};
    };

    std::shared_ptr<OperationMetrics> GetOrCreate(const std::string &operation);
    void FlushLoop();
    void FlushWindow();
    void WriteSamples();

    int instance_id_;
    int interval_ms_;
    std::string output_dir_;
    mutable std::mutex operations_mutex_;
    std::map<std::string, std::shared_ptr<OperationMetrics>> operations_;
    std::atomic<uint64_t> verify_failures_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> recording_{false};
    std::thread flush_thread_;
    std::atomic<int64_t> start_time_us_{0};
    std::atomic<int64_t> end_time_us_{0};
};

}  // namespace mooncake::kvtest
