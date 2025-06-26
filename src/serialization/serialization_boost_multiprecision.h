#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <string>

#include <boost/variant/variant.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>
#include <boost/mpl/empty.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/front.hpp>
#include <boost/mpl/pop_front.hpp>
#include "serialization.h"
#include "serialization/serialization_fwd.h"  // Required for serializer template


namespace serialization {

template <class Archive>
struct serializer<Archive, boost::multiprecision::uint128_t>
{
  static bool serialize(Archive& ar, boost::multiprecision::uint128_t& v)
  {
    std::string str;
    if (Archive::is_saving::value)
    {
      str = v.str();
    }

    bool result = do_serialize(ar, str);

    if (!Archive::is_saving::value && result)
    {
      try {
        v = boost::multiprecision::uint128_t(str);
      } catch (...) {
        return false;
      }
    }

    return result;
  }
};

} // namespace serialization
