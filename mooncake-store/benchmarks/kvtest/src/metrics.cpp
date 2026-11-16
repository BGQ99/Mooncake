#include "metrics.h"

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#else
#include <json/json.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace mooncake::kvtest {
namespace {

int64_t SteadyNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double Percentile(const std::vector<double> &sorted, double percentile) {
    if (sorted.empty()) return 0;
    const double rank = percentile * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(rank);
    const size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double fraction = rank - lower;
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

Json::Value LatencyJson(std::vector<double> values) {
    Json::Value result(Json::objectValue);
    if (values.empty()) {
        result["count"] = Json::UInt64(0);
        return result;
    }
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double value : values) sum += value;
    result["count"] = static_cast<Json::UInt64>(values.size());
    result["avg_ms"] = sum / values.size();
    result["min_ms"] = values.front();
    result["p50_ms"] = Percentile(values, 0.50);
    result["p90_ms"] = Percentile(values, 0.90);
    result["p99_ms"] = Percentile(values, 0.99);
    result["p999_ms"] = Percentile(values, 0.999);
    result["p9999_ms"] = Percentile(values, 0.9999);
    result["max_ms"] = values.back();
    return result;
}

std::string JsonString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

}  // namespace

MetricsCollector::MetricsCollector(int instance_id, int interval_ms,
                                   std::string output_dir)
    : instance_id_(instance_id),
      interval_ms_(interval_ms),
      output_dir_(std::move(output_dir)) {}

MetricsCollector::~MetricsCollector() { Stop(); }

void MetricsCollector::Start() {
    std::filesystem::create_directories(output_dir_);
    const auto csv = std::filesystem::path(output_dir_) / "latency_timeseries.csv";
    if (!std::filesystem::exists(csv)) {
        std::ofstream output(csv);
        output << "elapsed_ms,instance_id,operation,count,success,failure,bytes,"
                  "avg_ms,p50_ms,p90_ms,p99_ms,p999_ms,p9999_ms,max_ms\n";
    }
    start_time_us_.store(SteadyNowUs(), std::memory_order_release);
    end_time_us_.store(0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    flush_thread_ = std::thread([this] { FlushLoop(); });
}

void MetricsCollector::Reset() {
    std::lock_guard<std::mutex> operations_lock(operations_mutex_);
    for (const auto &[name, metric] : operations_) {
        (void)name;
        std::lock_guard<std::mutex> latency_lock(metric->latency_mutex);
        metric->total.store(0, std::memory_order_relaxed);
        metric->success.store(0, std::memory_order_relaxed);
        metric->failure.store(0, std::memory_order_relaxed);
        metric->bytes.store(0, std::memory_order_relaxed);
        metric->window_latencies.clear();
        metric->samples.clear();
        metric->flushed_success = 0;
        metric->flushed_failure = 0;
        metric->flushed_bytes = 0;
    }
    verify_failures_.store(0, std::memory_order_relaxed);
    start_time_us_.store(SteadyNowUs(), std::memory_order_release);
    end_time_us_.store(0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
}

void MetricsCollector::Freeze() {
    if (!recording_.exchange(false, std::memory_order_acq_rel)) return;
    // Wait for records that passed the pre-lock recording check so the frozen
    // elapsed time and counters describe the same completed interval.
    std::lock_guard<std::mutex> operations_lock(operations_mutex_);
    for (const auto &[name, metric] : operations_) {
        (void)name;
        std::lock_guard<std::mutex> latency_lock(metric->latency_mutex);
    }
    end_time_us_.store(SteadyNowUs(), std::memory_order_release);
}

double MetricsCollector::ElapsedSeconds() const {
    const int64_t start = start_time_us_.load(std::memory_order_acquire);
    const int64_t end = end_time_us_.load(std::memory_order_acquire);
    const int64_t now = end == 0 ? SteadyNowUs() : end;
    return start == 0
               ? 0.0
               : std::max<int64_t>(0, now - start) / 1000000.0;
}

void MetricsCollector::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    Freeze();
    if (flush_thread_.joinable()) flush_thread_.join();
    FlushWindow();
    WriteSamples();
    WriteSummary();
}

std::shared_ptr<MetricsCollector::OperationMetrics>
MetricsCollector::GetOrCreate(const std::string &operation) {
    thread_local std::unordered_map<
        const MetricsCollector *,
        std::unordered_map<std::string, std::shared_ptr<OperationMetrics>>>
        cache;
    auto &collector_cache = cache[this];
    auto cached = collector_cache.find(operation);
    if (cached != collector_cache.end()) return cached->second;
    std::lock_guard<std::mutex> lock(operations_mutex_);
    auto &entry = operations_[operation];
    if (!entry) entry = std::make_shared<OperationMetrics>();
    collector_cache.emplace(operation, entry);
    return entry;
}

void MetricsCollector::Record(const std::string &operation, double latency_ms,
                              bool success, uint64_t bytes) {
    if (!recording_.load(std::memory_order_acquire)) return;
    const int64_t recorded_us = SteadyNowUs();
    auto metric = GetOrCreate(operation);
    std::lock_guard<std::mutex> lock(metric->latency_mutex);
    if (!recording_.load(std::memory_order_acquire)) return;
    metric->total.fetch_add(1, std::memory_order_relaxed);
    (success ? metric->success : metric->failure)
        .fetch_add(1, std::memory_order_relaxed);
    metric->bytes.fetch_add(bytes, std::memory_order_relaxed);
    metric->window_latencies.push_back(latency_ms);
    const int64_t start = start_time_us_.load(std::memory_order_acquire);
    metric->samples.push_back({std::max<int64_t>(0, recorded_us - start),
                               latency_ms, success, bytes});
}

void MetricsCollector::RecordVerifyFailure() {
    if (!recording_.load(std::memory_order_acquire)) return;
    verify_failures_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<MetricSnapshot> MetricsCollector::SnapshotCounts() const {
    std::vector<MetricSnapshot> snapshots;
    std::lock_guard<std::mutex> lock(operations_mutex_);
    snapshots.reserve(operations_.size());
    for (const auto &[name, metric] : operations_) {
        snapshots.push_back({name,
                             metric->total.load(std::memory_order_relaxed),
                             metric->success.load(std::memory_order_relaxed),
                             metric->failure.load(std::memory_order_relaxed),
                             metric->bytes.load(std::memory_order_relaxed)});
    }
    return snapshots;
}

std::string MetricsCollector::StatsJson() const {
    Json::Value root(Json::objectValue);
    root["instance_id"] = instance_id_;
    root["verify_failures"] = static_cast<Json::UInt64>(
        verify_failures_.load(std::memory_order_relaxed));
    Json::Value operations(Json::objectValue);
    std::lock_guard<std::mutex> lock(operations_mutex_);
    for (const auto &[name, metric] : operations_) {
        Json::Value op(Json::objectValue);
        op["total"] = static_cast<Json::UInt64>(
            metric->total.load(std::memory_order_relaxed));
        op["success"] = static_cast<Json::UInt64>(
            metric->success.load(std::memory_order_relaxed));
        op["failure"] = static_cast<Json::UInt64>(
            metric->failure.load(std::memory_order_relaxed));
        op["bytes"] = static_cast<Json::UInt64>(
            metric->bytes.load(std::memory_order_relaxed));
        operations[name] = std::move(op);
    }
    root["operations"] = std::move(operations);
    return JsonString(root);
}

void MetricsCollector::FlushLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(interval_ms_);
        while (running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (running_.load(std::memory_order_acquire)) FlushWindow();
    }
}

void MetricsCollector::FlushWindow() {
    std::vector<std::pair<std::string, std::shared_ptr<OperationMetrics>>> ops;
    {
        std::lock_guard<std::mutex> lock(operations_mutex_);
        for (const auto &entry : operations_) ops.push_back(entry);
    }
    const auto elapsed = static_cast<int64_t>(ElapsedSeconds() * 1000.0);
    std::ofstream output(std::filesystem::path(output_dir_) /
                             "latency_timeseries.csv",
                         std::ios::app);
    for (const auto &[name, metric] : ops) {
        std::vector<double> latencies;
        uint64_t success = 0;
        uint64_t failure = 0;
        uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(metric->latency_mutex);
            latencies.swap(metric->window_latencies);
            const uint64_t total_success =
                metric->success.load(std::memory_order_relaxed);
            const uint64_t total_failure =
                metric->failure.load(std::memory_order_relaxed);
            const uint64_t total_bytes =
                metric->bytes.load(std::memory_order_relaxed);
            success = total_success - metric->flushed_success;
            failure = total_failure - metric->flushed_failure;
            bytes = total_bytes - metric->flushed_bytes;
            metric->flushed_success = total_success;
            metric->flushed_failure = total_failure;
            metric->flushed_bytes = total_bytes;
        }
        if (latencies.empty()) continue;
        const auto stats = LatencyJson(latencies);
        output << elapsed << ',' << instance_id_ << ',' << name << ','
               << latencies.size() << ',' << success << ',' << failure << ','
               << bytes << ',' << std::fixed << std::setprecision(6)
               << stats.get("avg_ms", 0).asDouble() << ','
               << stats.get("p50_ms", 0).asDouble() << ','
               << stats.get("p90_ms", 0).asDouble() << ','
               << stats.get("p99_ms", 0).asDouble() << ','
               << stats.get("p999_ms", 0).asDouble() << ','
               << stats.get("p9999_ms", 0).asDouble() << ','
               << stats.get("max_ms", 0).asDouble() << '\n';
    }
}

void MetricsCollector::WriteSamples() {
    std::vector<std::pair<std::string, std::shared_ptr<OperationMetrics>>> ops;
    {
        std::lock_guard<std::mutex> lock(operations_mutex_);
        for (const auto &entry : operations_) ops.push_back(entry);
    }
    std::ofstream output(std::filesystem::path(output_dir_) /
                         "latency_samples.csv");
    output << "elapsed_us,instance_id,operation,request_index,latency_ms,"
              "status,bytes\n";
    for (const auto &[name, metric] : ops) {
        std::vector<RequestSample> samples;
        {
            std::lock_guard<std::mutex> lock(metric->latency_mutex);
            samples = metric->samples;
        }
        std::stable_sort(samples.begin(), samples.end(),
                         [](const auto &a, const auto &b) {
                             return a.elapsed_us < b.elapsed_us;
                         });
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &sample = samples[i];
            output << sample.elapsed_us << ',' << instance_id_ << ',' << name
                   << ',' << i << ',' << std::fixed << std::setprecision(6)
                   << sample.latency_ms << ','
                   << (sample.success ? "OK" : "ERR") << ','
                   << sample.bytes << '\n';
        }
    }
}

void MetricsCollector::WriteSummary() {
    Json::Value root(Json::objectValue);
    root["instance_id"] = instance_id_;
    root["elapsed_seconds"] = ElapsedSeconds();
    root["verify_failures"] = static_cast<Json::UInt64>(
        verify_failures_.load(std::memory_order_relaxed));
    Json::Value operations(Json::objectValue);
    std::lock_guard<std::mutex> lock(operations_mutex_);
    for (const auto &[name, metric] : operations_) {
        std::vector<double> latencies;
        {
            std::lock_guard<std::mutex> latency_lock(metric->latency_mutex);
            latencies.reserve(metric->samples.size());
            for (const auto &sample : metric->samples)
                latencies.push_back(sample.latency_ms);
        }
        auto op = LatencyJson(std::move(latencies));
        op["total"] = static_cast<Json::UInt64>(
            metric->total.load(std::memory_order_relaxed));
        op["success"] = static_cast<Json::UInt64>(
            metric->success.load(std::memory_order_relaxed));
        op["failure"] = static_cast<Json::UInt64>(
            metric->failure.load(std::memory_order_relaxed));
        op["bytes"] = static_cast<Json::UInt64>(
            metric->bytes.load(std::memory_order_relaxed));
        operations[name] = std::move(op);
    }
    root["operations"] = std::move(operations);
    std::ofstream output(std::filesystem::path(output_dir_) / "summary.json");
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    output << Json::writeString(builder, root) << '\n';
}

}  // namespace mooncake::kvtest
