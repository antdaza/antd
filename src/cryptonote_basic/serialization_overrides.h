#pragma once
#include <string>
#include <vector>
#include "common/util.h"           // epee::to_hex
#include "string_tools.h"          // epee::string_tools
#include "cryptonote_basic/tx_extra.h"

namespace epee
{
  namespace serialization
  {
    template <class Archive>
    void serialize_value(Archive& ar, cryptonote::tx_extra_nonce& nonce)
    {
      if (ar.is_store())
      {
        std::string hex = epee::to_hex::string(
          epee::span<const uint8_t>(reinterpret_cast<const uint8_t*>(nonce.nonce.data()), nonce.nonce.size()));
        ar.store(hex);
      }
      else
      {
        std::string hex;
        ar.load(hex);

        std::string bin_str;
        if (!epee::string_tools::parse_hexstr_to_binbuff(hex, bin_str))
          throw std::runtime_error("Invalid hex string in tx_extra_nonce");

        nonce.nonce = bin_str;
      }
    }
  }
}
