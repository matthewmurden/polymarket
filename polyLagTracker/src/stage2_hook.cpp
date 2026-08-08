#include "stage2_hook.hpp"

#include "logging.hpp"

// Placeholder implementation: logs and returns. See the header comment for
// what a real Stage 2 (funding-graph tracing) implementation should do
// here instead. Deliberately not implemented as part of Stage 1.
void onWalletFlagged(const std::string& walletAddress, const AnomalyScore& score) {
    logging::info("stage2_hook: wallet " + walletAddress + " crossed the anomaly threshold "
                  "(total_score=" + std::to_string(score.total_score) +
                  ") -- Stage 2 funding-graph tracing not yet implemented, this is a placeholder");
}
