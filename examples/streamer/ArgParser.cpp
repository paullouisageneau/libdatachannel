/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "ArgParser.hpp"
#include <iostream>

ArgParser::ArgParser(std::vector<std::pair<std::string, std::string>> options, std::vector<std::pair<std::string, std::string>> flags) {
    for(auto option: options) {
        this->options.insert(option.first);
        this->options.insert(option.second);
        shortToLongMap.emplace(option.first, option.second);
        shortToLongMap.emplace(option.second, option.second);
    }
    for(auto flag: flags) {
        this->flags.insert(flag.first);
        this->flags.insert(flag.second);
        shortToLongMap.emplace(flag.first, flag.second);
        shortToLongMap.emplace(flag.second, flag.second);
    }
}

std::optional<std::string> ArgParser::toKey(std::string prefixedKey) {
    if (prefixedKey.find("--") == 0) {
        return prefixedKey.substr(2, prefixedKey.length());
    } else if (prefixedKey.find("-") == 0) {
        return prefixedKey.substr(1, prefixedKey.length());
    } else {
        return std::nullopt;
    }
}

bool ArgParser::parse(int argc, char **argv, std::function<bool (std::string, std::string)> onOption, std::function<bool (std::string)> onFlag) {
    std::optional<std::string> currentOption = std::nullopt;
    for(int i = 1; i < argc; i++) {
        std::string current = argv[i];
        auto optKey = toKey(current);
        if (!currentOption.has_value() && optKey.has_value() && flags.find(optKey.value()) != flags.end()) {
            auto check = onFlag(shortToLongMap.at(optKey.value()));
            if (!check) {
                return false;
            }
        } else if (!currentOption.has_value() && optKey.has_value() && options.find(optKey.value()) != options.end()) {
            currentOption = optKey.value();
        } else if (currentOption.has_value()) {
            auto check = onOption(shortToLongMap.at(currentOption.value()), current);
            if (!check) {
                return false;
            }
            currentOption = std::nullopt;
        } else {
            std::cerr << "Unrecognized option " << current << std::endl;
            return false;
        }
    }
    if (currentOption.has_value()) {
        std::cerr << "Missing value for " << currentOption.value() << std::endl;
        return false;
    }
    return true;
}
