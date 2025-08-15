#include "article_ownership.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "common/scoped_message_writer.h"
#include "string_tools.h"
#include <sqlite/sqlite3.h>
#include <stdexcept>

namespace cryptonote {

ArticleOwnershipTracker::ArticleOwnershipTracker(const std::string& db_path, network_type nettype)
    : db_(nullptr), db_path_(db_path), nettype_(nettype) {
}

ArticleOwnershipTracker::~ArticleOwnershipTracker() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool ArticleOwnershipTracker::init() {
  tools::msg_writer() << "Debug: Initializing article_ownership.db at " << db_path_;
  int rc = sqlite3_open(db_path_.c_str(), &db_);
  if (rc != SQLITE_OK) {
    tools::msg_writer() << "Failed to open article_ownership.db: " << sqlite3_errmsg(db_);
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  if (!create_tables()) {
    tools::msg_writer() << "Failed to create tables in article_ownership.db";
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  tools::msg_writer() << "Debug: article_ownership.db initialized successfully";
  return true;
}

bool ArticleOwnershipTracker::create_tables() {
  const char* sql = R"(
    CREATE TABLE IF NOT EXISTS article_ownership (
      article_tx_hash TEXT PRIMARY KEY,
      owner_address TEXT NOT NULL
    );
  )";
  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    tools::msg_writer() << "Failed to create table: " << err_msg;
    sqlite3_free(err_msg);
    return false;
  }
  return true;
}

std::string ArticleOwnershipTracker::get_owner(const std::string& article_tx_hash) {
  if (!db_) {
    tools::msg_writer() << "Error: Database not initialized";
    return "";
  }

  sqlite3_stmt* stmt;
  const char* sql = "SELECT owner_address FROM article_ownership WHERE article_tx_hash = ?;";
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    tools::msg_writer() << "Failed to prepare SELECT statement: " << sqlite3_errmsg(db_);
    return "";
  }

  crypto::hash hash;
  if (!epee::string_tools::hex_to_pod(article_tx_hash, hash)) {
    tools::msg_writer() << "Failed to convert article_tx_hash to hash: " << article_tx_hash;
    sqlite3_finalize(stmt);
    return "";
  }
  std::string hash_hex = epee::string_tools::pod_to_hex(hash);
  sqlite3_bind_text(stmt, 1, hash_hex.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  std::string result;
  if (rc == SQLITE_ROW) {
    const char* owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (owner) {
      result = owner;
    }
  } else if (rc != SQLITE_DONE) {
    tools::msg_writer() << "Failed to execute SELECT statement: " << sqlite3_errmsg(db_);
  }

  sqlite3_finalize(stmt);
  return result;
}

bool ArticleOwnershipTracker::update_ownership(const std::string& article_tx_hash, const std::string& new_owner_address) {
  if (!db_) {
    tools::msg_writer() << "Error: Database not initialized";
    return false;
  }

  cryptonote::address_parse_info addr_info;
  if (!cryptonote::get_account_address_from_str(addr_info, nettype_, new_owner_address)) {
    tools::msg_writer() << "Invalid new owner address: " << new_owner_address;
    return false;
  }

  sqlite3_stmt* stmt;
  const char* sql = "INSERT OR REPLACE INTO article_ownership (article_tx_hash, owner_address) VALUES (?, ?);";
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    tools::msg_writer() << "Failed to prepare INSERT statement: " << sqlite3_errmsg(db_);
    return false;
  }

  crypto::hash hash;
  if (!epee::string_tools::hex_to_pod(article_tx_hash, hash)) {
    tools::msg_writer() << "Failed to convert article_tx_hash to hash: " << article_tx_hash;
    sqlite3_finalize(stmt);
    return false;
  }
  std::string hash_hex = epee::string_tools::pod_to_hex(hash);
  sqlite3_bind_text(stmt, 1, hash_hex.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, new_owner_address.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    tools::msg_writer() << "Failed to execute INSERT statement: " << sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return false;
  }

  sqlite3_finalize(stmt);
  tools::msg_writer() << "Debug: Updated ownership for article " << article_tx_hash << " to " << new_owner_address;
  return true;
}

}
