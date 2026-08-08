#pragma once

#include <optional>
#include <string>
#include <vector>

// One WS-subscribable outcome token found via category-based discovery,
// with enough context to log/debug which tag(s) and market it came from.
struct CategoryDiscoveredAsset {
    std::string asset_id;      // clobTokenId -- what the WS market channel subscribes with
    std::string condition_id;
    std::string event_title;
    std::string tag_slug;      // which queried tag this event was found under (first match)
};

// Fetches every currently-OPEN market across `tagSlugs` via Polymarket's
// Gamma API: GET /events?tag_slug=<slug>&closed=false, paginated (the
// endpoint silently caps `limit` at 100 regardless of what's requested --
// confirmed live, same as polyEventCalibration's earlier finding). Events
// are deduplicated by event id (the same event can carry several of the
// queried tags).
//
// IMPORTANT, confirmed live and NOT assumed: an event's own top-level
// `closed=false` does NOT guarantee every market nested inside it is still
// open -- a multi-outcome event can stay "open" while individual markets
// within it have already resolved. This function filters at the MARKET
// level (`market.closed == false`), not just the event level, for exactly
// that reason.
//
// Each open market's `clobTokenIds` (a JSON-encoded array, 2 entries for
// an ordinary Yes/No market, more for NegRisk multi-outcome markets) is
// fully flattened -- one CategoryDiscoveredAsset per outcome token, not
// per market -- so that trades on ANY outcome of an open market are
// captured, not just whichever side happens to be listed first.
//
// Unlike discoverActiveAssets() (volume-based), this does NOT rank or cap
// by trade volume or count: it returns every open market matching any of
// the tags, however many that is. Confirmed live (see README "Category-
// based market discovery") that this can be a LARGE number -- tens of
// thousands of outcome tokens for a broad politics/geopolitics/elections
// tag set. Callers must check the returned size and handle it explicitly
// (log it, warn if large), never assume it's small.
std::optional<std::vector<CategoryDiscoveredAsset>> discoverAssetsByCategory(
    const std::vector<std::string>& tagSlugs, long timeoutMs);
