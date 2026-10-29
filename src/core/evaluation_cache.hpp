#pragma once

#include "cache_controls.hpp"
#include "evaluator.hpp"

#include <deque>
#include <list>
#include <unordered_map>

namespace shrink::detail {
// Per reduction, ordinary non-preserving outcomes only. FIFO eviction bounds
// retained candidate bytes and entry/container overhead independently.
class EvaluationCache {
public:
    explicit EvaluationCache(CacheControls controls) : controls_(controls) {}
    std::optional<CandidateOutcome> find(std::string_view text, CancellationToken cancellation) const;
    void remember(std::string_view text, CandidateOutcome outcome, CancellationToken cancellation);
private:
    struct Entry { std::string text; CandidateOutcome outcome; };
    std::optional<std::uint64_t> fingerprint(std::string_view text, CancellationToken cancellation) const;
    CacheControls controls_;
    std::unordered_map<std::uint64_t, std::list<Entry>> buckets_;
    std::deque<std::uint64_t> order_;
    std::size_t text_bytes_ = 0;
};
}
