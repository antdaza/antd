
#pragma once

#include "string_tools.h" // for epee::to_hex::string
#include <sstream>
#include <stdexcept>
#include <string>
#include <boost/uuid/uuid.hpp>
#include <boost/variant.hpp>
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "ringct/rctOps.h"
#include "serialization/keyvalue_serialization.h"

#define STAKING_REQUIREMENT_LOCK_BLOCKS_EXCESS          20
#define STAKING_PORTIONS                                UINT64_C(0xfffffffffffffffc)
#define MAX_NUMBER_OF_CONTRIBUTORS                      4
#define MIN_PORTIONS                                    (STAKING_PORTIONS / MAX_NUMBER_OF_CONTRIBUTORS)

static_assert(STAKING_PORTIONS % MAX_NUMBER_OF_CONTRIBUTORS == 0,  "Use a multiple of four, so that it divides easily by max number of contributors.");
static_assert(STAKING_PORTIONS % 2 == 0, "Use a multiple of two, so that it divides easily by two contributors.");
static_assert(STAKING_PORTIONS % 3 == 0, "Use a multiple of three, so that it divides easily by three contributors.");

#define STAKING_AUTHORIZATION_EXPIRATION_WINDOW         (60*60*24*7*2)  // 2 weeks

#define UPTIME_PROOF_FREQUENCY_IN_SECONDS               (60*60)

#define UPTIME_PROOF_BUFFER_IN_SECONDS                  (5*60) // The acceptable window of time to accept a peer's uptime proof from its reported t>#define UPTIME_PROOF_FREQUENCY_IN_SECONDS               (60*60)
#define UPTIME_PROOF_MAX_TIME_IN_SECONDS                (UPTIME_PROOF_FREQUENCY_IN_SECONDS * 2 + UPTIME_PROOF_BUFFER_IN_SECONDS)

static constexpr size_t MAX_ARTICLE_TITLE_LEN = 128;
static constexpr size_t MAX_ARTICLE_CONTENT_LEN = 2048;
static constexpr size_t MAX_ARTICLE_PUBLISHER_LEN = 64;

static constexpr uint64_t ARTICLE_POSTING_FEE = 5000 * COIN; // 5000 ANTD

namespace cryptonote {

inline std::string print_transaction_as_json_safe(const transaction& tx) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"version\": " << tx.version << ",\n";
  oss << "  \"unlock_time\": " << tx.unlock_time << ",\n";

  // vin
  oss << "  \"vin\": [\n";
  for (size_t i = 0; i < tx.vin.size(); ++i) {
    const txin_v& in = tx.vin[i];
    oss << "    {\n";

    if (const txin_to_key* tk = boost::get<txin_to_key>(&in)) {
      oss << "      \"type\": \"txin_to_key\",\n";
      oss << "      \"amount\": " << tk->amount << ",\n";
      oss << "      \"key_image\": \"" << epee::string_tools::pod_to_hex(tk->k_image) << "\",\n";
      oss << "      \"key_offsets\": [";
      for (size_t j = 0; j < tk->key_offsets.size(); ++j) {
        oss << tk->key_offsets[j];
        if (j + 1 < tk->key_offsets.size()) oss << ", ";
      }
      oss << "]\n";
    }
    else if (const txin_gen* gen = boost::get<txin_gen>(&in)) {
      oss << "      \"type\": \"coinbase\",\n";
      oss << "      \"height\": " << gen->height << "\n";
    }
    else {
      oss << "      \"type\": \"unknown\"\n";
    }

    oss << "    }";
    if (i + 1 < tx.vin.size()) oss << ",";
    oss << "\n";
  }
  oss << "  ],\n";

  // vout
  oss << "  \"vout\": [\n";
  for (size_t i = 0; i < tx.vout.size(); ++i) {
    const tx_out& out = tx.vout[i];
    oss << "    {\n";
    oss << "      \"amount\": " << out.amount << ",\n";

    if (const txout_to_key* tk = boost::get<txout_to_key>(&out.target)) {
      oss << "      \"type\": \"txout_to_key\",\n";
      oss << "      \"key\": \"" << epee::string_tools::pod_to_hex(tk->key) << "\"\n";
    }
    else {
      oss << "      \"type\": \"unknown\"\n";
    }

    oss << "    }";
    if (i + 1 < tx.vout.size()) oss << ",";
    oss << "\n";
  }
  oss << "  ],\n";

  // Extra
  oss << "  \"extra_size\": " << tx.extra.size() << ",\n";
  if (!tx.extra.empty()) {
   std::string extra_hex = epee::to_hex::string(epee::span<const uint8_t>(tx.extra.data(), tx.extra.size()));
    oss << "  \"extra\": \"" << extra_hex << "\"\n";
  } else {
    oss << "  \"extra\": null\n";
  }

  oss << "}\n";
  return oss.str();
}

} // namespace cryptonote

