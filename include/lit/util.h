#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lit {

std::string urlencode(std::string_view raw);
std::string urldecode(std::string_view raw);

std::vector<std::string_view>  //
str_split(std::string_view str, std::string_view delimiter);

}  // namespace lit
