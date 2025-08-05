#include "metadata_type.h"
#include "serialization/keyvalue_serialization.h"
#include <sstream>

template <class Archive>
bool article_metadata::serialize(Archive& ar)
{
  ar.begin_object();
  
  // Serialize bool
  ar.tag("success");
  ar.serialize_varint(success);
  
  // Serialize strings
  ar.tag("error");
  ar.begin_string();
  ar.stream() << error;
  ar.end_string();
  
  ar.tag("title");
  ar.begin_string();
  ar.stream() << title;
  ar.end_string();
  
  ar.tag("content");
  ar.begin_string();
  ar.stream() << content;
  ar.end_string();
  
  ar.tag("publisher");
  ar.begin_string();
  ar.stream() << publisher;
  ar.end_string();
  
  // Serialize binary data
  ar.tag("serialized_blob");
  ar.serialize_blob(const_cast<void*>(static_cast<const void*>(serialized_blob.data())), 
                    serialized_blob.size(), "\"");
  
  ar.tag("content_hash");
  ar.serialize_blob(const_cast<void*>(static_cast<const void*>(&content_hash)), 
                    sizeof(content_hash), "\"");
  
  // Serialize optional calculator_metadata
  bool has_calc = calc.is_initialized();
  ar.tag("has_calc");
  ar.serialize_varint(has_calc);
  if (has_calc) {
    calculator_metadata& calc_ref = *calc;
    ar.tag("calc");
    ar.begin_object();
    
    ar.tag("operation");
    ar.begin_string();
    ar.stream() << calc_ref.operation;
    ar.end_string();
    
    // Convert doubles to strings
    std::ostringstream oss1, oss2, oss3;
    oss1 << std::fixed << calc_ref.operand1;
    oss2 << std::fixed << calc_ref.operand2;
    oss3 << std::fixed << calc_ref.result;
    
    ar.tag("operand1");
    ar.begin_string();
    ar.stream() << oss1.str();
    ar.end_string();
    
    ar.tag("operand2");
    ar.begin_string();
    ar.stream() << oss2.str();
    ar.end_string();
    
    ar.tag("result");
    ar.begin_string();
    ar.stream() << oss3.str();
    ar.end_string();
    
    ar.end_object();
  }
  
  // Serialize optional ballot_metadata
  bool has_ballot = ballot.is_initialized();
  ar.tag("has_ballot");
  ar.serialize_varint(has_ballot);
  if (has_ballot) {
    ballot_metadata& ballot_ref = *ballot;
    ar.tag("ballot");
    ar.begin_object();
    
    ar.tag("operation");
    ar.begin_string();
    ar.stream() << ballot_ref.operation;
    ar.end_string();
    
    ar.tag("ballot_id");
    ar.begin_string();
    ar.stream() << ballot_ref.ballot_id;
    ar.end_string();
    
    ar.tag("title");
    ar.begin_string();
    ar.stream() << ballot_ref.title;
    ar.end_string();
    
    ar.tag("voter_id");
    ar.begin_string();
    ar.stream() << ballot_ref.voter_id;
    ar.end_string();
    
    ar.tag("selected_option");
    ar.begin_string();
    ar.stream() << ballot_ref.selected_option;
    ar.end_string();
    
    ar.tag("timestamp");
    ar.serialize_varint(ballot_ref.timestamp);
    
    ar.end_object();
  }
  
  ar.end_object();
  return true;
}

// Explicit template instantiation
template bool article_metadata::serialize<json_archive<true>>(json_archive<true>&);
