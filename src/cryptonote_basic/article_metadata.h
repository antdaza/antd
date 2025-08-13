#pragma once
#include <string>
#include "cryptonote_basic.h"
#include "cryptonote_basic/metadata_type.h"

namespace cryptonote {
cryptonote::article_metadata set_article_to_tx_extra(const std::string& title, const std::string& content, const std::string& publisher);
}
