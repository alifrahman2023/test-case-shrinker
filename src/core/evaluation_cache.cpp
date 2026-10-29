#include "evaluation_cache.hpp"

namespace shrink::detail {
namespace {
constexpr std::size_t polling_bytes = 16 * 1024;
bool same_text(std::string_view left, std::string_view right, CancellationToken cancellation) {
    if (left.size() != right.size()) return false;
    for (std::size_t offset = 0; offset < left.size(); offset += polling_bytes) {
        if (cancellation.stop_requested()) return false;
        if (left.substr(offset, polling_bytes) != right.substr(offset, polling_bytes)) return false;
    }
    return true;
}
}
std::optional<std::uint64_t> EvaluationCache::fingerprint(std::string_view text, CancellationToken cancellation) const {
    if (!controls_.enabled || controls_.max_entries == 0 || text.size() > controls_.max_text_bytes) return {};
    if (controls_.forced_fingerprint) return controls_.forced_fingerprint;
    // FNV-1a is only an index. Exact candidate identity always decides a hit.
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index % polling_bytes == 0 && cancellation.stop_requested()) return {};
        hash ^= static_cast<unsigned char>(text[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}
std::optional<CandidateOutcome> EvaluationCache::find(std::string_view text, CancellationToken cancellation) const {
    const auto key = fingerprint(text, cancellation);
    if (!key) return {};
    const auto bucket = buckets_.find(*key);
    if (bucket == buckets_.end()) return {};
    for (const auto& entry : bucket->second) {
        if (cancellation.stop_requested()) return {};
        if (same_text(entry.text, text, cancellation)) return entry.outcome;
    }
    return {};
}
void EvaluationCache::remember(std::string_view text, CandidateOutcome outcome, CancellationToken cancellation) {
    if (outcome == CandidateOutcome::preserved) return;
    const auto key = fingerprint(text, cancellation);
    if (!key || cancellation.stop_requested()) return;
    while (order_.size() >= controls_.max_entries || text.size() > controls_.max_text_bytes - text_bytes_) {
        if (cancellation.stop_requested()) return;
        const auto oldest = order_.front();
        auto bucket = buckets_.find(oldest);
        text_bytes_ -= bucket->second.front().text.size();
        bucket->second.pop_front();
        if (bucket->second.empty()) buckets_.erase(bucket);
        order_.pop_front();
    }
    buckets_[*key].push_back({std::string(text), outcome});
    order_.push_back(*key);
    text_bytes_ += text.size();
}
}
