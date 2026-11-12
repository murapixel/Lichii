// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace FrontendCommon {

/// A user-named group of mods. Mods are identified by their folder name and listed in the order
/// the user arranged them.
struct ModCategory {
    std::string name;
    std::vector<std::string> mods;
};

/// How a game's mods are grouped in the add-ons tab. This is presentation only: the patch loader
/// never reads it, so load order and the enabled/disabled state are unaffected by it.
struct ModCategories {
    std::vector<ModCategory> categories;
    std::vector<std::string> uncategorized;
};

/// Reads and writes the category mapping of one title as JSON. Names of mods that are not on disk
/// are kept, so a mod that comes back (SD card, renamed folder) returns to its previous slot.
class ModCategoryStore {
public:
    explicit ModCategoryStore(std::filesystem::path file_);
    explicit ModCategoryStore(u64 title_id);

    /// The per-title mapping file, outside every directory the loader scans.
    static std::filesystem::path FileFor(u64 title_id);

    /// Trims leading and trailing whitespace, which is how names are compared and stored.
    static std::string NormalizeName(const std::string& name);

    /// A missing or unreadable file yields an empty mapping.
    ModCategories Load() const;

    /// Writes through a temporary file so a crash cannot leave a half-written mapping.
    bool Save(const ModCategories& data) const;

private:
    std::filesystem::path file;
};

} // namespace FrontendCommon
