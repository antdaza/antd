#include "article_metadata.h"
#include "common/i18n.h"
#include "string_tools.h"
#include "cryptonote_config.h"
#include "metadata_type.h"
#include "tx_extra.h"

namespace cryptonote {

  cryptonote::article_metadata set_article_to_tx_extra(const std::string& title, const std::string& content, const std::string& publisher) {
  article_metadata result;
  if (title.size() > 128 || content.size() > 4000 || publisher.size() > 64) {
    result.error = "Metadata too large";
    return result;
  }
  if (title.empty() || content.empty()) {
    result.error = "Missing title or content";
    return result;
  }
  tx_extra_article_info article_info = {title, content, publisher.empty() ? "Unknown" : publisher};

  //message_writer() << tr("Debug: set_article_to_tx_extra: Manually serializing");
  std::vector<uint8_t> serialized;
  try {
    size_t total_size = 1 + article_info.title.size() + 2 + article_info.content.size() + 1 + article_info.publisher.size();
    if (total_size + 4 > TX_EXTRA_NONCE_MAX_COUNT - 2) { // Account for ARTC (4), tag (1), length (1)
      result.error = "Serialized data too large: " + std::to_string(total_size + 4) + " bytes, max " + std::to_string(TX_EXTRA_NONCE_MAX_COUNT - 2);
    //  message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
      return result;
    }

    serialized.reserve(total_size);
    serialized.push_back(static_cast<uint8_t>(article_info.title.size()));
    serialized.insert(serialized.end(), article_info.title.begin(), article_info.title.end());
    serialized.push_back(static_cast<uint8_t>(article_info.content.size() >> 8)); // High byte
    serialized.push_back(static_cast<uint8_t>(article_info.content.size() & 0xFF)); // Low byte
    serialized.insert(serialized.end(), article_info.content.begin(), article_info.content.end());
    serialized.push_back(static_cast<uint8_t>(article_info.publisher.size()));
    serialized.insert(serialized.end(), article_info.publisher.begin(), article_info.publisher.end());

    result.serialized_blob = {'A', 'R', 'T', 'C'};
    result.serialized_blob.insert(result.serialized_blob.end(), serialized.begin(), serialized.end());
    //message_writer() << tr("Debug: set_article_to_tx_extra: Serialization completed, size: ") << result.serialized_blob.size();
    //message_writer() << tr("Debug: set_article_to_tx_extra: Serialized data (hex): ") << epee::string_tools::buff_to_hex_nodelim(result.serialized_blob);
  } catch (const std::exception& e) {
    result.error = "Serialization exception: " + std::string(e.what());
    //message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
    return result;
  }

  if (result.serialized_blob.empty()) {
    result.error = "Serialized data empty";
    //message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
    return result;
  }

 // message_writer() << tr("Debug: set_article_to_tx_extra: Computing content hash");
  crypto::cn_fast_hash(content.data(), content.size(), result.content_hash);

  result.success = true;
  //message_writer() << tr("Debug: set_article_to_tx_extra: Completed");
  return result;
}

}
