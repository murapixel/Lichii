// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "frontend_common/mod_category_store.h"

namespace FrontendCommon {
namespace {

constexpr int FormatVersion = 1;

std::vector<std::string> ReadModList(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }
    for (const auto& entry : value) {
        if (entry.is_string()) {
            const auto name = ModCategoryStore::NormalizeName(entry.get<std::string>());
            if (!name.empty()) {
                out.push_back(name);
            }
        }
    }
    return out;
}

} // namespace

ModCategoryStore::ModCategoryStore(std::filesystem::path file_) : file{std::move(file_)} {}

ModCategoryStore::ModCategoryStore(u64 title_id) : file{FileFor(title_id)} {}

std::filesystem::path ModCategoryStore::FileFor(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "mod_categories" /
           fmt::format("{:016X}.json", title_id);
}

std::string ModCategoryStore::NormalizeName(const std::string& name) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    std::size_t first = 0;
    while (first < name.size() && is_space(name[first])) {
        ++first;
    }
    std::size_t last = name.size();
    while (last > first && is_space(name[last - 1])) {
        --last;
    }
    return name.substr(first, last - first);
}

ModCategories ModCategoryStore::Load() const {
    ModCategories out;
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return out;
    }

    try {
        const auto root = nlohmann::json::parse(stream, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return {};
        }
        // Every key is optional: a hand-edited or older file must not be able to read past the
        // end of the object, which is what the const subscript operator does on a missing key.
        const auto categories = root.value("categories", nlohmann::json::array());
        if (categories.is_array()) {
            for (const auto& entry : categories) {
                if (!entry.is_object()) {
                    continue;
                }
                const auto name = NormalizeName(entry.value("name", std::string{}));
                if (name.empty()) {
                    continue;
                }
                // A duplicated category name would be ambiguous in the UI; keep the first one.
                const auto duplicate = std::ranges::any_of(out.categories, [&](const auto& c) {
                    return c.name == name;
                });
                if (duplicate) {
                    continue;
                }
                out.categories.push_back(
                    {name, ReadModList(entry.value("mods", nlohmann::json::array()))});
            }
        }
        out.uncategorized = ReadModList(root.value("uncategorized", nlohmann::json::array()));
    } catch (const std::exception&) {
        return {};
    }

    return out;
}

bool ModCategoryStore::Save(const ModCategories& data) const {
    nlohmann::json root;
    root["version"] = FormatVersion;
    root["categories"] = nlohmann::json::array();
    for (const auto& category : data.categories) {
        root["categories"].push_back({{"name", category.name}, {"mods", category.mods}});
    }
    root["uncategorized"] = data.uncategorized;

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        return false;
    }

    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream stream{tmp, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return false;
        }
        stream << root.dump(2) << '\n';
        if (!stream) {
            return false;
        }
    }

    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // Windows refuses to rename over an existing file on some standard library versions.
        ec.clear();
        std::filesystem::remove(file, ec);
        ec.clear();
        std::filesystem::rename(tmp, file, ec);
    }
    return !ec;
}

} // namespace FrontendCommon
