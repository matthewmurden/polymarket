#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// Thin JSON-RPC client for a Polygon node (Alchemy/Infura/public RPC --
// anything that speaks standard Ethereum JSON-RPC over HTTPS). One-shot
// libcurl POST per call, matching the style of http_bench.cpp in
// polyAreaTesting rather than pulling in a heavier RPC library.
class PolygonRpcClient {
public:
    PolygonRpcClient(std::string rpcUrl, long timeoutMs);

    // Low-level: POSTs {"jsonrpc":"2.0","method":method,"params":params}
    // and fills *result with the "result" field. Returns false on transport
    // failure or a JSON-RPC "error" response (logged either way).
    bool call(const std::string& method, const nlohmann::json& params, nlohmann::json* result);

    // eth_getTransactionByHash. nullopt on transport failure; on success,
    // the result is JSON null (as a json object holding `nullptr`) if the
    // node doesn't know the hash yet, or an object with at least "hash"
    // and (once mined) "blockNumber"/"blockHash".
    std::optional<nlohmann::json> getTransactionByHash(const std::string& txHash);

    // eth_getBlockByNumber. hexBlockNumber must be "0x..."-prefixed (or the
    // literal "latest").
    std::optional<nlohmann::json> getBlockByNumber(const std::string& hexBlockNumber, bool fullTx = false);

    std::optional<uint64_t> getLatestBlockNumber();

    // eth_getTransactionReceipt. nullopt on transport failure; on success,
    // the result is JSON null if the node doesn't know the hash yet (or it
    // hasn't been mined), or an object with "blockNumber", "logs" (the full,
    // unfiltered array of event logs emitted by the tx), "status", etc. once
    // mined. Logs here are NOT pre-filtered by address/topic0 -- callers
    // filter for the events they care about.
    std::optional<nlohmann::json> getTransactionReceipt(const std::string& txHash);

    // eth_getLogs over [fromBlock, toBlock], filtered to one contract
    // address and topic0 (event signature hash). Returns the raw "result"
    // array of log objects.
    std::optional<nlohmann::json> getLogs(const std::string& address, const std::string& topic0,
                                           uint64_t fromBlock, uint64_t toBlock);

private:
    std::string rpcUrl_;
    long timeoutMs_;
};

// "0x1a2b..." -> integer. Safe for values that fit in uint64_t (block
// numbers, timestamps); not intended for full 256-bit token amounts.
uint64_t hexToU64(const std::string& hex);

std::string u64ToHex(uint64_t v);

// Best-effort decode of a 32-byte (64 hex char) word from event log `data`
// into a double. Amounts here are 1e6-scaled USDC / share counts, well
// within double's exact-integer range, so this is precise for realistic
// trade sizes; it is NOT a general 256-bit integer parser and will lose
// precision on inputs anywhere near 2^53.
double hexWordToDouble(const std::string& word64Hex);
