#include "viewer_pipeline.hpp"
#include "h264_decoder.hpp"
#include "receive_observer.hpp"
#include "vulkan_presenter.hpp"

#include <teleop/performance.hpp>
#include <teleop/reassembler.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace teleop {
namespace {

volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }

class SignalScope {
public:
    SignalScope()
    {
        interrupted = 0;
        struct sigaction action{};
        action.sa_handler = interrupt_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &old_int_) != 0) throw std::runtime_error("sigaction SIGINT failed");
        if (sigaction(SIGTERM, &action, &old_term_) != 0) {
            sigaction(SIGINT, &old_int_, nullptr);
            throw std::runtime_error("sigaction SIGTERM failed");
        }
    }
    ~SignalScope()
    {
        sigaction(SIGINT, &old_int_, nullptr);
        sigaction(SIGTERM, &old_term_, nullptr);
    }
private:
    struct sigaction old_int_{};
    struct sigaction old_term_{};
};

class WakeFd {
public:
    WakeFd() : fd_(eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK))
    {
        if (fd_ < 0) throw std::runtime_error("eventfd failed");
    }
    ~WakeFd() { close(fd_); }
    int fd() const { return fd_; }
    void notify() const
    {
        const std::uint64_t one = 1U;
        ssize_t result;
        do { result = write(fd_, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
        // EAGAIN means the eventfd is already readable; it still wakes poll.
    }
private:
    int fd_;
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct DisplayFrame {
    FramePtr frame;
    std::uint64_t decoded_ns{};
    std::uint64_t ready_ns{};
    std::uint64_t ordinal{};
};

struct EncodedFrame {
    CompletedFrame frame;
    std::uint64_t ready_ns{};
};

struct StreamStatistics {
    std::uint64_t enqueued{};
    std::size_t peak_depth{};
    std::size_t bytes{};
    std::size_t peak_bytes{};
    std::uint64_t overflows{};
    std::uint64_t published{};
    std::uint64_t replaced{};
    std::uint64_t displayed{};
    std::uint64_t last_displayed{};
    DecodeStatistics decode;
};

struct SharedState {
    std::mutex mutex;
    std::condition_variable changed;
    bool stop{};
    bool rx_done{};
    std::size_t ready_workers{};
    std::array<bool, 2U> decoder_done{};
    std::exception_ptr error;
    std::array<std::deque<EncodedFrame>, 2U> queues;
    std::array<DisplayFrame, 2U> pending;
    std::array<StreamStatistics, 2U> streams;
};

class MailboxSink final : public DecodedFrameSink {
public:
    MailboxSink(SharedState& shared, std::size_t stream) : shared_(shared), stream_(stream) {}
    std::string_view timing_stage() const override { return "mailbox_publish"; }
    void present(const AVFrame& frame) override
    {
        FramePtr retained{av_frame_clone(&frame)};
        if (!retained) throw std::runtime_error("could not retain decoded mailbox frame");
        DisplayFrame discarded;
        {
            std::lock_guard lock{shared_.mutex};
            if (shared_.stop) return;
            auto& stats = shared_.streams[stream_];
            auto& pending = shared_.pending[stream_];
            if (pending.frame) ++stats.replaced;
            discarded = std::move(pending);
            pending = {.frame = std::move(retained), .decoded_ns = viewer_monotonic_ns(),
                       .ready_ns = frame.pts > 0 ? static_cast<std::uint64_t>(frame.pts) : 0U,
                       .ordinal = ++stats.published};
        }
        // Releasing hardware frame references must not hold the RX/queue lock.
        shared_.changed.notify_all();
    }
private:
    SharedState& shared_;
    std::size_t stream_;
};

class Pipeline {
public:
    Pipeline(const ViewerPipelineOptions& options, VulkanPresenter* presenter)
        : options_(options), presenter_(presenter), receiver_(options.port), observer_(receiver_.fd()) {}
    ~Pipeline() { stop(); join(); }

    void run()
    {
        if (!options_.transport_only) {
            for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
                workers_[stream] = std::jthread{[this, stream] { decode(stream); }};
            }
            std::unique_lock lock{shared_.mutex};
            shared_.changed.wait(lock, [this] {
                return shared_.stop || shared_.ready_workers == options_.stream_count;
            });
            if (shared_.error) std::rethrow_exception(shared_.error);
        }
        rx_ = std::jthread{[this] { receive(); }};
        std::string reason = "limit";
        try {
            while (true) {
                if (presenter_ != nullptr) {
                    presenter_->poll_events();
                    if (presenter_->close_requested()) { reason = "window-close"; stop(); break; }
                }
                report_queues();
                if (interrupted != 0) { reason = "signal"; stop(); break; }
                std::array<DisplayFrame, 2U> batch;
                bool done = false;
                bool failed = false;
                {
                    std::unique_lock lock{shared_.mutex};
                    shared_.changed.wait_for(lock, std::chrono::milliseconds{5}, [this] {
                        return shared_.stop || complete() || has_pending();
                    });
                    failed = shared_.error != nullptr;
                    for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
                        batch[stream] = std::move(shared_.pending[stream]);
                        shared_.pending[stream] = {};
                    }
                    done = complete();
                }
                if (failed) { reason = "error"; stop(); break; }
                bool changed = false;
                for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
                    if (!batch[stream].frame) continue;
                    presenter_->update_stream(static_cast<std::uint16_t>(stream), *batch[stream].frame);
                    changed = true;
                }
                if (changed) {
                    const auto begin = viewer_monotonic_ns();
                    if (options_.diagnostic_present_delay_ms != 0U) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{options_.diagnostic_present_delay_ms});
                    }
                    const auto previous_presentations = presenter_->presented_frames();
                    presenter_->render_latest_frames();
                    const bool presented = presenter_->presented_frames() != previous_presentations;
                    const auto end = viewer_monotonic_ns();
                    record_stage("host.display.batch_call", begin, end);
                    for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
                        if (!batch[stream].frame) continue;
                        const auto prefix = "host.stream" + std::to_string(stream) + ".";
                        record_stage(prefix + "decoded_to_present_begin", batch[stream].decoded_ns, begin);
                        record_stage(prefix + "au_ready_to_present_return", batch[stream].ready_ns, end);
                        if (!presented) continue;
                        std::lock_guard lock{shared_.mutex};
                        ++shared_.streams[stream].displayed;
                        shared_.streams[stream].last_displayed = batch[stream].ordinal;
                    }
                }
                if (done) break;
            }
        } catch (...) {
            fail(std::current_exception());
            reason = "error";
        }
        join();
        print(reason);
        if (shared_.error) std::rethrow_exception(shared_.error);
    }

private:
    // Called only with shared_.mutex held.
    bool complete() const
    {
        if (!shared_.rx_done) return false;
        if (options_.transport_only) return true;
        for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
            if (!shared_.decoder_done[stream]) return false;
        }
        return true;
    }
    bool has_pending() const
    {
        for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
            if (shared_.pending[stream].frame) return true;
        }
        return false;
    }
    void report_queues()
    {
        if (options_.queue_report_ms == 0U) return;
        const auto now = viewer_monotonic_ns();
        if (now - last_queue_report_ns_ < static_cast<std::uint64_t>(options_.queue_report_ms) * 1'000'000U) return;
        last_queue_report_ns_ = now;
        struct Snapshot {
            std::size_t depth{}, bytes{}, peak{};
            std::uint64_t oldest{}, mailbox_age{}, decoded{}, replaced{};
            bool pending{};
        };
        std::array<Snapshot, 2U> snapshots{};
        {
            std::lock_guard lock{shared_.mutex};
            for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
                const auto& queue = shared_.queues[stream];
                const auto& stats = shared_.streams[stream];
                const auto& mailbox = shared_.pending[stream];
                snapshots[stream] = {.depth = queue.size(), .bytes = stats.bytes, .peak = stats.peak_depth,
                    .oldest = queue.empty() ? 0U : (now >= queue.front().ready_ns ? now - queue.front().ready_ns : 0U),
                    .mailbox_age = mailbox.frame ? (now >= mailbox.decoded_ns ? now - mailbox.decoded_ns : 0U) : 0U,
                    .decoded = stats.decode.decoded_frames, .replaced = stats.replaced,
                    .pending = mailbox.frame != nullptr};
            }
        }
        for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
            const auto& sample = snapshots[stream];
            std::printf("queue-sample: mono_ns=%llu stream=%zu depth=%zu bytes=%zu peak_depth=%zu oldest_au_ms=%.6f mailbox_pending=%u mailbox_age_ms=%.6f decoded=%llu mailbox_replaced=%llu\n",
                static_cast<unsigned long long>(now), stream, sample.depth, sample.bytes, sample.peak,
                static_cast<double>(sample.oldest) / 1'000'000.0, static_cast<unsigned>(sample.pending),
                static_cast<double>(sample.mailbox_age) / 1'000'000.0,
                static_cast<unsigned long long>(sample.decoded), static_cast<unsigned long long>(sample.replaced));
        }
        std::fflush(stdout);
    }
    void stop()
    {
        { std::lock_guard lock{shared_.mutex}; shared_.stop = true; }
        wake_.notify();
        shared_.changed.notify_all();
    }
    void fail(std::exception_ptr error)
    {
        {
            std::lock_guard lock{shared_.mutex};
            if (!shared_.error) shared_.error = error;
            shared_.stop = true;
        }
        wake_.notify();
        shared_.changed.notify_all();
    }
    void join()
    {
        if (rx_.joinable()) rx_.join();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
    void receive()
    {
        ThreadObservation observation{"rx"};
        const auto cpu_begin = viewer_thread_cpu_ns();
        try {
            configure_worker("rx", options_.scheduling.rx_cpu, options_.scheduling.rx_rr_priority);
            std::array<FrameReassembler, 2U> reassemblers;
            std::array<std::uint64_t, 2U> previous_ready{};
            std::array<std::byte, kMaxDatagramSize> buffer{};
            auto last_received = viewer_monotonic_ns();
            ReceiveBatch batch{options_.receive_batch_size};
            const auto process_datagram = [&](const ReceivedDatagram& received, std::span<const std::byte> payload) {
                last_received = received.received_ns;
                if (received.truncated) { ++statistics_.malformed_datagrams; return; }
                const auto datagram = payload.first(static_cast<std::size_t>(received.size));
                const auto header = parse_header(datagram);
                if (!header || header->stream_id >= options_.stream_count) { ++statistics_.malformed_datagrams; return; }
                auto completed = reassemblers[header->stream_id].accept(datagram, statistics_, received.received_ns);
                if (!completed) return;
                const auto ready_ns = viewer_monotonic_ns();
                const auto stream = completed->stream_id;
                const auto prefix = "host.stream" + std::to_string(stream) + ".";
                record_stage(prefix + "reassembly", completed->first_receive_ns, ready_ns);
                record_stage(prefix + "au_interval", previous_ready[stream], ready_ns);
                previous_ready[stream] = ready_ns;
                if (options_.same_clock && ready_ns >= completed->capture_time_ns) {
                    latency_.add_sample(ready_ns - completed->capture_time_ns);
                }
                if (options_.transport_only) return;
                {
                    std::lock_guard lock{shared_.mutex};
                    if (shared_.stop) return;
                    auto& queue = shared_.queues[stream];
                    auto& stats = shared_.streams[stream];
                    if (queue.size() >= options_.queue_capacity) {
                        ++stats.overflows;
                        throw std::runtime_error("encoded queue capacity exceeded for stream " + std::to_string(stream) +
                                                 "; refusing to discard H.264 reference pictures");
                    }
                    stats.bytes += completed->payload.size();
                    queue.push_back({std::move(*completed), ready_ns});
                    ++stats.enqueued;
                    stats.peak_depth = std::max(stats.peak_depth, queue.size());
                    stats.peak_bytes = std::max(stats.peak_bytes, stats.bytes);
                }
                shared_.changed.notify_all();
            };
            while (options_.max_frames == 0U || statistics_.completed_frames < options_.max_frames) {
                const auto now = viewer_monotonic_ns();
                const auto timeout_ns = static_cast<std::uint64_t>(options_.timeout_ms) * 1'000'000U;
                if (now - last_received >= timeout_ns) throw std::runtime_error("network inactivity timeout");
                const auto remaining_ms = (timeout_ns - (now - last_received) + 999'999U) / 1'000'000U;
                const auto timeout = static_cast<int>(std::min(remaining_ms, static_cast<std::uint64_t>(2'147'483'647U)));
                std::array<pollfd, 2U> descriptors{{{receiver_.fd(), POLLIN, 0}, {wake_.fd(), POLLIN, 0}}};
                ++rx_poll_calls_;
                const int ready = poll(descriptors.data(), descriptors.size(), timeout);
                if (ready < 0) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error(std::string{"poll: "} + std::strerror(errno));
                }
                if ((descriptors[1].revents & POLLIN) != 0) break;
                if (ready == 0) continue;
                if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    throw std::runtime_error("UDP poll error");
                }
                if (options_.receive_batch_size == 1U) {
                    const auto received = observer_.receive(receiver_.fd(), buffer);
                    if (received.size < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        throw std::runtime_error(std::string{"recvmsg: "} + std::strerror(errno));
                    }
                    process_datagram(received, buffer);
                    continue;
                }
                // Drain current packets without waiting to fill a batch. A
                // partial return means recvmmsg already reached the empty queue.
                // Bound each drain round so flood traffic cannot starve cancel.
                std::size_t drained = 0U;
                while (drained < 256U && (options_.max_frames == 0U || statistics_.completed_frames < options_.max_frames)) {
                    { std::lock_guard lock{shared_.mutex}; if (shared_.stop) break; }
                    const int count = observer_.receive_batch(receiver_.fd(), batch);
                    if (count < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        throw std::runtime_error(std::string{"recvmmsg: "} + std::strerror(errno));
                    }
                    const auto process_begin = performance_time_ns();
                    for (const auto& packet : batch.packets()) {
                        process_datagram(packet.result, packet.payload);
                        if (options_.max_frames != 0U && statistics_.completed_frames >= options_.max_frames) break;
                    }
                    record_stage("host.rx.batch_process", process_begin, performance_time_ns());
                    drained += static_cast<std::size_t>(count);
                    if (static_cast<std::size_t>(count) < options_.receive_batch_size) break;
                }
            }
        } catch (...) { fail(std::current_exception()); }
        {
            std::lock_guard lock{shared_.mutex};
            shared_.rx_done = true;
            rx_cpu_ns_ = viewer_thread_cpu_ns() - cpu_begin;
        }
        shared_.changed.notify_all();
        print_software_metrics();
    }

    void decode(std::size_t stream)
    {
        const std::string_view role = stream == 0U ? "dec0" : "dec1";
        ThreadObservation observation{role};
        try {
            configure_worker(role, options_.scheduling.decoder_cpu[stream]);
            MailboxSink sink{shared_, stream};
            H264VaapiDecoder decoder{options_.vaapi_device, presenter_ != nullptr ? &sink : nullptr,
                                     !options_.software_decode, static_cast<std::uint16_t>(stream)};
            H264AnnexBParser parser{decoder};
            std::optional<bool> contract;
            const auto prefix = "host.stream" + std::to_string(stream) + ".";
            {
                std::lock_guard lock{shared_.mutex};
                ++shared_.ready_workers;
            }
            shared_.changed.notify_all();
            bool cancelled = false;
            while (true) {
                EncodedFrame next;
                {
                    std::unique_lock lock{shared_.mutex};
                    shared_.changed.wait(lock, [this, stream] {
                        return shared_.stop || shared_.rx_done || !shared_.queues[stream].empty();
                    });
                    if (shared_.stop) { cancelled = true; break; }
                    auto& queue = shared_.queues[stream];
                    if (queue.empty()) break;
                    next = std::move(queue.front());
                    queue.pop_front();
                    shared_.streams[stream].bytes -= next.frame.payload.size();
                }
                const auto begin = viewer_monotonic_ns();
                record_stage(prefix + "au_queue_wait", next.ready_ns, begin);
                if (options_.diagnostic_decode_delay_ms != 0U) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{options_.diagnostic_decode_delay_ms});
                }
                const bool complete_au = has_flag(next.frame.flags, PacketFlags::kCompleteAccessUnit);
                if (contract && *contract != complete_au) throw std::runtime_error("H.264 AU contract changed midstream; restart viewer");
                if (!contract) {
                    contract = complete_au;
                    std::printf("viewer stream %zu: input=%s\n", stream,
                                complete_au && !options_.force_stream_parser ? "complete-au" : "annex-b-stream");
                }
                if (complete_au && !options_.force_stream_parser) decoder.submit_access_unit(next.frame.payload, next.ready_ns);
                else parser.submit_chunk(next.frame.payload, next.ready_ns);
                record_stage(prefix + "parser_decode_call", begin, viewer_monotonic_ns());
                {
                    std::lock_guard lock{shared_.mutex};
                    shared_.streams[stream].decode = decoder.statistics();
                }
            }
            if (!cancelled) { parser.flush(); decoder.flush(); }
            {
                std::lock_guard lock{shared_.mutex};
                shared_.streams[stream].decode = decoder.statistics();
            }
        } catch (...) { fail(std::current_exception()); }
        {
            std::lock_guard lock{shared_.mutex};
            shared_.decoder_done[stream] = true;
        }
        shared_.changed.notify_all();
        print_software_metrics();
    }

    void print(const std::string& reason) const
    {
        std::printf("viewer: access_units=%llu datagrams=%llu bytes=%llu malformed=%llu duplicates=%llu dropped=%llu\n",
                    static_cast<unsigned long long>(statistics_.completed_frames), static_cast<unsigned long long>(statistics_.datagrams_received),
                    static_cast<unsigned long long>(statistics_.bytes_received), static_cast<unsigned long long>(statistics_.malformed_datagrams),
                    static_cast<unsigned long long>(statistics_.duplicate_datagrams), static_cast<unsigned long long>(statistics_.dropped_frames));
        std::printf("pipeline: mode=async queue_capacity=%zu reason=%s\n", options_.queue_capacity, reason.c_str());
        for (std::size_t stream = 0U; stream < options_.stream_count; ++stream) {
            const auto& stats = shared_.streams[stream];
            if (!options_.transport_only) {
                std::printf("viewer stream %zu: decoder: frames=%llu hardware_frames=%llu\n", stream,
                            static_cast<unsigned long long>(stats.decode.decoded_frames), static_cast<unsigned long long>(stats.decode.hardware_frames));
            }
            std::printf("queue: stream=%zu enqueued=%llu peak_depth=%zu peak_bytes=%zu overflows=%llu mailbox_replaced=%llu displayed=%llu last_decoded=%llu last_presented=%llu remaining_depth=%zu remaining_bytes=%zu\n",
                        stream, static_cast<unsigned long long>(stats.enqueued), stats.peak_depth, stats.peak_bytes,
                        static_cast<unsigned long long>(stats.overflows), static_cast<unsigned long long>(stats.replaced),
                        static_cast<unsigned long long>(stats.displayed), static_cast<unsigned long long>(stats.decode.decoded_frames),
                        static_cast<unsigned long long>(stats.last_displayed), shared_.queues[stream].size(), stats.bytes);
        }
        observer_.print();
        std::printf("rx-thread: batch_capacity=%zu poll_calls=%llu cpu_ms=%.6f\n", options_.receive_batch_size,
                    static_cast<unsigned long long>(rx_poll_calls_), static_cast<double>(rx_cpu_ns_) / 1'000'000.0);
        if (presenter_ != nullptr) std::printf("presenter: frames=%llu\n", static_cast<unsigned long long>(presenter_->presented_frames()));
        if (options_.same_clock && latency_.sample_count() != 0U) {
            std::printf("source-to-receive-ms: min=%.3f avg=%.3f max=%.3f samples=%llu\n", *latency_.minimum_ms(),
                        *latency_.average_ms(), *latency_.maximum_ms(), static_cast<unsigned long long>(latency_.sample_count()));
        }
        print_software_metrics();
    }

    std::uint64_t last_queue_report_ns_{};
    std::uint64_t rx_poll_calls_{};
    std::uint64_t rx_cpu_ns_{};
    ViewerPipelineOptions options_;
    VulkanPresenter* presenter_;
    UdpReceiver receiver_;
    ReceiveObserver observer_;
    WakeFd wake_;
    SharedState shared_;
    TransportStatistics statistics_;
    LatencyStatistics latency_;
    std::array<std::jthread, 2U> workers_;
    std::jthread rx_;
};

} // namespace

void run_viewer_pipeline(const ViewerPipelineOptions& options, VulkanPresenter* presenter)
{
    SignalScope signals;
    Pipeline pipeline{options, presenter};
    pipeline.run();
}
} // namespace teleop
