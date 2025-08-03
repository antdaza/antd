#pragma once

#include <boost/optional.hpp>

enum class metadata_type_t : uint8_t {
  ARTICLE = 0x01,
  CALCULATOR = 0x02,
  BALLOT = 0x03,
};

struct ballot_metadata {
  std::string operation;       // CREATE or VOTE
  std::string ballot_id;       // A unique string or hash (UUID, tx hash, etc.)
  std::string title;           // Only used for CREATE
  std::string voter_id;        // Optional: hashed address or pseudonym
  std::string selected_option; // Used for VOTE
  uint64_t timestamp;
};

struct calculator_metadata {
  std::string operation;
  double operand1;
  double operand2;
  double result;
};

struct article_metadata {
  bool success;
  std::string error;
  std::string serialized_blob;

  std::string title;
  std::string content;
  std::string publisher;
  crypto::hash content_hash;

  boost::optional<calculator_metadata> calc;
  boost::optional<ballot_metadata> ballot;

  article_metadata() : success(false), error(""), title(""), content(""), publisher(""), content_hash(crypto::null_hash) {}
};
