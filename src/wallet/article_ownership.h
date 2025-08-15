#pragma once

#include <string>
#include <sqlite/sqlite3.h>
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/account.h"

namespace cryptonote
{

class ArticleOwnershipTracker
{
public:
    ArticleOwnershipTracker(const std::string& db_path, network_type nettype);
    ~ArticleOwnershipTracker();

    // Open DB and create tables if needed
    bool init();

    // Return current owner of the article by tx hash (empty if not found)
    std::string get_owner(const std::string& article_tx_hash);

    // Insert or replace ownership record
    bool update_ownership(const std::string& article_tx_hash, const std::string& new_owner_address);

private:
    // Create required tables
    bool create_tables();

private:
    sqlite3* db_;                // SQLite connection handle
    std::string db_path_;        // Path to the database file
    network_type nettype_;       // Network type (mainnet/testnet/stagenet)
};

} // namespace cryptonote
