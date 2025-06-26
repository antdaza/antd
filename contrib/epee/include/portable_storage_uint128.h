#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <storages/portable_storage.h>
#include <string>

namespace epee::serialization
{
    inline bool set_value(portable_storage& ps, const std::string& name, const boost::multiprecision::uint128_t& value, hsection section)
    {
        std::string str = value.str();
        return ps.set_value(name, str, section);
    }

    inline bool get_value(portable_storage& ps, const std::string& name, boost::multiprecision::uint128_t& value, hsection section)
    {
        std::string str;
        if (!ps.get_value(name, str, section))
            return false;
        try {
            value = boost::multiprecision::uint128_t(str);
        } catch (...) {
            return false;
        }
        return true;
    }
}
