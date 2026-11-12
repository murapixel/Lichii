// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/mod_category_store.h"

namespace {

using FrontendCommon::ModCategories;
using FrontendCommon::ModCategoryStore;

struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() / "lichii-mod-category-test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path File() const {
        return path / "0400C3F00006E000.json";
    }
    std::filesystem::path path;
};

TEST_CASE("ModCategoryStore: names are trimmed", "[mod_category_store]") {
    REQUIRE(ModCategoryStore::NormalizeName("  Graphics\t") == "Graphics");
    REQUIRE(ModCategoryStore::NormalizeName("\n") == "");
    REQUIRE(ModCategoryStore::NormalizeName("60 FPS") == "60 FPS");
}

TEST_CASE("ModCategoryStore: a missing file is an empty mapping", "[mod_category_store]") {
    TempDir dir;
    const ModCategoryStore store{dir.File()};
    const auto loaded = store.Load();
    REQUIRE(loaded.categories.empty());
    REQUIRE(loaded.uncategorized.empty());
}

TEST_CASE("ModCategoryStore: round trip keeps order and empty categories", "[mod_category_store]") {
    TempDir dir;
    const ModCategoryStore store{dir.File()};

    ModCategories data;
    data.categories.push_back({"Graphics", {"Reshade", "60 FPS"}});
    data.categories.push_back({"Empty", {}});
    data.uncategorized = {"Cheat folder"};

    REQUIRE(store.Save(data));
    const auto loaded = store.Load();
    REQUIRE(loaded.categories.size() == 2);
    REQUIRE(loaded.categories[0].name == "Graphics");
    REQUIRE(loaded.categories[0].mods == std::vector<std::string>{"Reshade", "60 FPS"});
    REQUIRE(loaded.categories[1].name == "Empty");
    REQUIRE(loaded.categories[1].mods.empty());
    REQUIRE(loaded.uncategorized == std::vector<std::string>{"Cheat folder"});
}

TEST_CASE("ModCategoryStore: names of absent mods survive a round trip", "[mod_category_store]") {
    TempDir dir;
    const ModCategoryStore store{dir.File()};

    ModCategories data;
    data.categories.push_back({"Graphics", {"On SD card"}});
    REQUIRE(store.Save(data));

    // Nothing is created on disk: the store only records names, never mod contents.
    REQUIRE(!std::filesystem::exists(dir.path / "On SD card"));
    const auto loaded = store.Load();
    REQUIRE(loaded.categories[0].mods == std::vector<std::string>{"On SD card"});
}

TEST_CASE("ModCategoryStore: corrupt input falls back to empty", "[mod_category_store]") {
    TempDir dir;
    {
        std::ofstream stream{dir.File(), std::ios::binary | std::ios::trunc};
        stream << "{not json";
    }
    const ModCategoryStore store{dir.File()};
    REQUIRE(store.Load().categories.empty());
}

TEST_CASE("ModCategoryStore: missing keys load instead of reading past the object",
          "[mod_category_store]") {
    TempDir dir;
    const ModCategoryStore store{dir.File()};

    SECTION("no categories key") {
        {
            std::ofstream stream{dir.File(), std::ios::binary | std::ios::trunc};
            stream << R"({"version":1,"uncategorized":["A"]})";
        }
        const auto loaded = store.Load();
        REQUIRE(loaded.categories.empty());
        REQUIRE(loaded.uncategorized == std::vector<std::string>{"A"});
    }

    SECTION("category without a mods key") {
        {
            std::ofstream stream{dir.File(), std::ios::binary | std::ios::trunc};
            stream << R"({"version":1,"categories":[{"name":"G"}],"uncategorized":[]})";
        }
        const auto loaded = store.Load();
        REQUIRE(loaded.categories.size() == 1);
        REQUIRE(loaded.categories[0].name == "G");
        REQUIRE(loaded.categories[0].mods.empty());
    }

    SECTION("no uncategorized key") {
        {
            std::ofstream stream{dir.File(), std::ios::binary | std::ios::trunc};
            stream << R"({"version":1,"categories":[{"name":"G","mods":["A"]}]})";
        }
        const auto loaded = store.Load();
        REQUIRE(loaded.categories.size() == 1);
        REQUIRE(loaded.categories[0].mods == std::vector<std::string>{"A"});
        REQUIRE(loaded.uncategorized.empty());
    }
}

TEST_CASE("ModCategoryStore: duplicate category names keep the first", "[mod_category_store]") {
    TempDir dir;
    {
        std::ofstream stream{dir.File(), std::ios::binary | std::ios::trunc};
        stream << R"({"version":1,"categories":[)";
        stream << R"({"name":"Graphics","mods":["A"]},)";
        stream << R"({"name":"Graphics","mods":["B"]}],"uncategorized":[]})";
    }
    const ModCategoryStore store{dir.File()};
    const auto loaded = store.Load();
    REQUIRE(loaded.categories.size() == 1);
    REQUIRE(loaded.categories[0].mods == std::vector<std::string>{"A"});
}

TEST_CASE("ModCategoryStore: saving twice replaces the file", "[mod_category_store]") {
    TempDir dir;
    const ModCategoryStore store{dir.File()};

    ModCategories data;
    data.categories.push_back({"Graphics", {"A"}});
    REQUIRE(store.Save(data));
    data.categories.clear();
    data.categories.push_back({"Gameplay", {"B"}});
    REQUIRE(store.Save(data));

    const auto loaded = store.Load();
    REQUIRE(loaded.categories.size() == 1);
    REQUIRE(loaded.categories[0].name == "Gameplay");
    REQUIRE(!std::filesystem::exists(dir.path / "0400C3F00006E000.json.tmp"));
}

} // namespace
