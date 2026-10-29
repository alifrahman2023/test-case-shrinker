#include "presentation.hpp"
#include <algorithm>
#include <ostream>

namespace shrink::cli {
namespace {
std::string preview(std::string_view bytes, std::size_t limit) {
    std::string text = "\"";
    constexpr char hex[] = "0123456789abcdef";
    std::size_t consumed = 0;
    for (unsigned char byte : bytes) {
        std::string escaped;
        if (byte == '\n') escaped = "\\n";
        else if (byte == '\r') escaped = "\\r";
        else if (byte == '\t') escaped = "\\t";
        else if (byte == '"' || byte == '\\') { escaped += '\\'; escaped += byte; }
        else if (byte < 32 || byte >= 127) {
            escaped = "\\x"; escaped += hex[byte >> 4]; escaped += hex[byte & 15];
        } else escaped += byte;
        // The cap applies to rendered bytes, including escape expansion.
        if (text.size() - 1 + escaped.size() > limit) break;
        text += escaped;
        ++consumed;
    }
    text += '"';
    if (consumed < bytes.size()) text += " [presentation truncated]";
    return text;
}
void evidence(std::ostream& errors, const char* label, const Evaluation& evaluation) {
    const auto& difference = *evaluation.disagreement;
    errors << label << ": both programs succeeded; first differing token: " << difference.token_position << '\n';
    auto token = [&](const char* role, const std::string& bytes, TokenLocation location) {
        errors << "  " << role << ": " << (location.end_of_output ? "end of output"
            : preview(std::string_view(bytes).substr(location.offset, location.length), 80)) << '\n';
    };
    token("solution", evaluation.execution.programs.solution.stdout_bytes, difference.solution);
    token("reference", evaluation.execution.programs.reference.stdout_bytes, difference.reference);
}
void execution_facts(std::ostream& errors, const char* role, const ExecutionEvidence& execution) {
    errors << role << ':';
    if (execution.exit_code) errors << " exit " << *execution.exit_code;
    if (execution.signal) errors << " signal " << *execution.signal << " (observed termination; cause not established)";
    if (!execution.launched) errors << " not launched";
    else if (!execution.setup_complete) errors << " setup not confirmed";
    for (auto reason : execution.stop_reasons) {
        switch (reason) {
        case StopReason::timeout: errors << "; timeout"; break;
        case StopReason::stdout_overflow: errors << "; stdout overflow"; break;
        case StopReason::cancellation: errors << "; interrupted"; break;
        case StopReason::peer_failure: errors << "; peer stop"; break;
        case StopReason::runner_error: errors << "; runner error stop"; break;
        }
    }
    errors << "; output " << (execution.output_complete ? "complete" : "incomplete")
           << "; cleanup " << (execution.cleanup_complete ? "complete" : "incomplete") << '\n';
    if (!execution.stderr_bytes.empty()) errors << "  stderr: " << preview(execution.stderr_bytes, 4096) << '\n';
    if (execution.stderr_truncated) errors << "  stderr retention truncated at 64 KiB (separate from presentation limit).\n";
}
const char* outcome_text(RunOutcome outcome) {
    switch (outcome) {
    case RunOutcome::invalid_request: return "invalid request";
    case RunOutcome::baseline_rejected: return "baseline rejected";
    case RunOutcome::completed: return "completed";
    case RunOutcome::evaluation_limit: return "evaluation limit reached";
    case RunOutcome::time_limit: return "overall time limit reached";
    case RunOutcome::cancelled: return "caller cancellation";
    case RunOutcome::operational_stop: return "operational error";
    }
}
}
int present_result(const ReductionResult& result, const std::filesystem::path& destination,
                   std::ostream& output, std::ostream& errors) {
    if (result.baseline.status == BaselineStatus::agreement)
        errors << "No wrong-answer disagreement found for this input.\n";
    if (result.best) {
        evidence(errors, "Baseline", *result.baseline.evaluation);
        evidence(errors, "Final", *result.best->evidence);
    }
    if (!result.best && result.baseline.evaluation) {
        if (result.baseline.status == BaselineStatus::solution_unsuccessful) errors << "Baseline: unsuccessful solution execution.\n";
        if (result.baseline.status == BaselineStatus::reference_unsuccessful) errors << "Baseline: unsuccessful reference execution (inconclusive).\n";
        if (result.baseline.status == BaselineStatus::interrupted) errors << "Baseline verification interrupted.\n";
        const auto& programs = result.baseline.evaluation->execution.programs;
        execution_facts(errors, "solution", programs.solution);
        execution_facts(errors, "reference", programs.reference);
    }
    if (result.operational_evaluation) {
        errors << "Candidate operational failure:\n";
        execution_facts(errors, "solution", result.operational_evaluation->execution.programs.solution);
        execution_facts(errors, "reference", result.operational_evaluation->execution.programs.reference);
    }
    errors << "Stopped: " << outcome_text(result.outcome) << ".\n";
    if (result.baseline.original_length) errors << "Original length: " << *result.baseline.original_length << '\n';
    if (result.best) errors << "Final length: " << result.best->values.size() << '\n';
    errors << "Baseline evaluations: " << result.statistics.baseline_evaluations
           << "; candidate evaluations: " << result.statistics.candidate_evaluations
           << "; acceptances: " << result.statistics.acceptances
           << "; lost executions: " << result.statistics.lost_executions
           << "; inconclusive executions: " << result.statistics.inconclusive_executions
           << "; elapsed: " << result.statistics.elapsed_ms << " ms\n";
    if (result.statistics.round == RoundStatus::not_started)
        errors << "No reduction round began; distinct inconclusive candidates: 0 (no round).\n";
    else
        errors << (result.statistics.round == RoundStatus::complete ? "Complete final round" : "Reduction round unfinished")
               << "; distinct inconclusive candidates: " << result.statistics.distinct_inconclusive_candidates
               << (result.statistics.round == RoundStatus::complete ? " (complete round).\n" : " (partial round).\n");
    if (result.statistics.round == RoundStatus::complete)
        errors << "Irreducible under supported local checks: every single-element removal; replacing all occurrences "
                  "of each distinct value, or one occurrence of a repeated value, with the simplest allowed value, "
                  "one unit toward zero, or its representable positive counterpart when negative. "
                  "Every applicable strictly smaller check was invalid, lost the wrong answer, or was inconclusive.\n"
                  "This is a local guarantee; smaller failing inputs may exist and preservation does not establish the same underlying bug.\n";
    else errors << "Further reduction may be possible; irreducibility was not established.\n";
    if (result.saving.last_saved_revision) errors << "Saved revision " << *result.saving.last_saved_revision << ": " << destination << '\n';
    if (!result.best) {
        std::error_code ignored;
        const bool exists = std::filesystem::exists(destination, ignored);
        errors << "No verified result was produced; " << destination
               << (exists ? " was left unchanged. A pre-existing file is not evidence from this run.\n"
                          : " was not created. No verified recovery input exists.\n");
    }
    if (result.observed_reasons.cancellation) errors << "Observed: caller cancellation.\n";
    if (result.observed_reasons.time_limit) errors << "Observed: overall time limit.\n";
    if (result.observed_reasons.evaluation_limit) errors << "Observed: evaluation limit.\n";
    for (const auto& pending : result.pending_ownership) {
        errors << "Unresolved " << (pending.program == ProgramRole::solution ? "solution" : "reference") << " ownership";
        if (pending.process_id) errors << "; process " << *pending.process_id;
        errors << "; awaiting reap: " << pending.awaiting_reap << "; open descriptors: " << pending.open_descriptors
               << "; uncertain releases: " << pending.uncertain_descriptors << ". Process exit cannot guarantee continued cleanup.\n";
    }
    errors << "Final save: " << (result.saving.final_outcome == FinalSaveOutcome::succeeded ? "succeeded"
        : result.saving.final_outcome == FinalSaveOutcome::failed ? "failed" : "not attempted") << '\n';
    for (const auto& failure : result.saving.failures) {
        errors << "Save error (" << (failure.kind == SaveKind::initial ? "initial" : failure.kind == SaveKind::periodic ? "periodic" : "final") << "): "
               << failure.error.operation << " (" << (failure.error.path.empty() ? destination : failure.error.path) << ')';
        if (failure.error.system_error) errors << ": " << failure.error.system_error.message();
        errors << '\n';
    }
    if (result.saving.final_outcome == FinalSaveOutcome::failed && result.best) {
        bool recovered = false;
        try {
            output << result.best->normalized_input;
            output.flush();
            recovered = static_cast<bool>(output);
        } catch (...) {}
        if (!recovered) errors << "Error: stdout recovery failed.\n";
        else errors << "Complete normalized best input emitted on stdout for recovery.\n";
    }
    for (const auto& error : result.errors) {
        if (error.kind == RunErrorKind::save) continue; // Detailed save context is above.
        errors << "Error: " << error.operation;
        if (error.validation) errors << ": " << error.validation->message;
        if (error.runner) {
            if (error.runner->program) errors << " (" << (*error.runner->program == ProgramRole::solution ? "solution" : "reference") << ')';
            if (error.runner->system_error) errors << ": " << std::error_code(error.runner->system_error, std::generic_category()).message();
        }
        errors << '\n';
    }
    if (!errors || !result.errors.empty()) return result.outcome == RunOutcome::invalid_request ? 2 : 1;
    if (result.outcome == RunOutcome::baseline_rejected) return 3;
    if (result.outcome == RunOutcome::cancelled) return 130;
    if (result.outcome == RunOutcome::evaluation_limit || result.outcome == RunOutcome::time_limit) return 4;
    return result.outcome == RunOutcome::completed ? 0 : 1;
}
}
