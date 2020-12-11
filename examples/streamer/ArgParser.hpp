/*
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ArgParser_hpp
#define ArgParser_hpp

#include <functional>
#include <vector>
#include <utility>
#include <string>
#include <set>
#include <unordered_map>
#include <optional>

struct ArgParser {
private:
    std::set<std::string> options{};
    std::set<std::string> flags{};
    std::unordered_map<std::string, std::string> shortToLongMap{};
public:
    ArgParser(std::vector<std::pair<std::string, std::string>> options, std::vector<std::pair<std::string, std::string>> flags);
    std::optional<std::string> toKey(std::string prefixedKey);
    bool parse(int argc, char **argv, std::function<bool (std::string, std::string)> onOption, std::function<bool (std::string)> onFlag);
};

#endif /* ArgParser_hpp */
