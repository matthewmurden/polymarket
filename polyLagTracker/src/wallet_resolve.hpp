#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "trade_event.hpp"

// Wallet resolution via direct on-chain OrderFilled log decoding.
//
// This replaced an earlier Data-API-based approach (GET
// /trades?market=<conditionId>, cross-referenced by transactionHash for its
// "proxyWallet" field) that only resolved ~18% of trades in practice: that
// endpoint's market-filtered index lags real time unpredictably, anywhere
// from ~instant to over 20 minutes, and no reasonable retry budget can paper
// over an eventual-consistency gap that large.
//
// Confirmed live before this replaced that approach (see README "Wallet
// address resolution"): the CTF Exchange V2 contract on Polygon
// (0xe111180000d2663c0091e4f400237545b87b996b) emits an OrderFilled event
// for every fill, and despite Polymarket's proxy-wallet/relayer
// architecture meaning the *transaction's* own from/to fields do NOT reveal
// the trader, the event's maker/taker fields ARE the real trading wallets
// (proxy wallets) -- validated against 83/83 known-correct (tx_hash,
// proxyWallet) pairs pulled from a live capture, with zero wrong answers
// and zero unresolved cases, before this replaced the Data-API lookup.
struct WalletResolution {
    bool resolved = false;
    std::string wallet_address;  // lowercase 0x-prefixed address, empty if unresolved
    std::string note;            // reason when unresolved, or how it was resolved
};

// Decodes the wallet for `trade` from a raw list of transaction logs --
// either a single tx's full eth_getTransactionReceipt "logs" array, or a
// block-range eth_getLogs result pre-filtered down to one transaction hash.
// Filters internally to the CTF Exchange V2 contract + the OrderFilled
// topic0 (so callers can pass an unfiltered receipt straight through), then
// matches by exact tokenId + price/size (within tolerance) against `trade`
// to find the specific fill among any other unrelated OrderFilled logs
// batched into the same settlement transaction, and picks maker vs. taker
// by comparing `trade.side` to the log's decoded side.
//
// That maker/taker rule -- and the fact that the CTF Exchange's redundant
// "mirror" log against itself (emitted for NegRisk mint/merge fills) needs
// no special-casing at all -- was traced directly from
// Polymarket/ctf-exchange-v2's Trading.sol source, not guessed: see
// wallet_resolve.cpp for the reasoning.
WalletResolution resolveWalletFromLogs(const nlohmann::json& logs, const TradeEvent& trade);
