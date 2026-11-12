// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <fmt/format.h>

#include <QBrush>
#include <QDesktopServices>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QTreeView>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "configuration/addon/mod_select_dialog.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "frontend_common/mod_manager.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/mod.h"
#include "ui_configure_per_game_addons.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_per_game_addons.h"

namespace {

const QString MimeType = QStringLiteral("application/x-lichii-addons");

/// Only real mod folders can be grouped. Updates, DLC, built-in patches, loose cheat files and
/// the SDMC entry keep their usual place in the list.
bool IsGroupableMod(const FileSys::Patch& patch) {
    if (patch.type != FileSys::PatchType::Mod || patch.location.empty()) {
        return false;
    }
    // Common::FS treats the string as UTF-8 on Windows; a narrow std::filesystem::path would
    // decode it as the ANSI code page and fail for non-ASCII mod folder names.
    return Common::FS::IsDir(patch.location);
}

/// The tree keeps its rows in the model, but drops are resolved against the saved mapping so a
/// drag can never change which mods load or in which order.
class AddonModel final : public QStandardItemModel {
public:
    using QStandardItemModel::QStandardItemModel;

    std::function<bool(const QMimeData*, const QModelIndex&, int)> on_drop;

    Qt::DropActions supportedDropActions() const override {
        return Qt::MoveAction;
    }

    QStringList mimeTypes() const override {
        return {MimeType};
    }

    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QStringList mods;
        QStringList categories;
        for (const auto& index : indexes) {
            if (index.column() != 0) {
                continue;
            }
            const int kind = index.data(ConfigurePerGameAddons::ENTRY_KIND).toInt();
            if (kind == ConfigurePerGameAddons::CategoryRow) {
                const auto name = index.data(ConfigurePerGameAddons::STORED_NAME).toString();
                if (!name.isEmpty() && !categories.contains(name)) {
                    categories << name;
                }
            } else if (kind == ConfigurePerGameAddons::ModRow) {
                if (!mods.contains(index.data(Qt::DisplayRole).toString())) {
                    mods << index.data(Qt::DisplayRole).toString();
                }
            }
        }

        QByteArray payload;
        if (!categories.isEmpty() && mods.isEmpty()) {
            payload = "C\n" + categories.join(QLatin1Char('\n')).toUtf8();
        } else if (!mods.isEmpty()) {
            payload = "M\n" + mods.join(QLatin1Char('\n')).toUtf8();
        } else {
            return nullptr;
        }

        auto* data = new QMimeData;
        data->setData(MimeType, payload);
        data->setText(QString::fromUtf8(payload));
        return data;
    }

    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int, int,
                         const QModelIndex&) const override {
        return action == Qt::MoveAction && data != nullptr && data->hasFormat(MimeType);
    }

    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int,
                      const QModelIndex& parent) override {
        if (action == Qt::IgnoreAction) {
            return true;
        }
        if (action != Qt::MoveAction || on_drop == nullptr) {
            return false;
        }
        return on_drop(data, parent, row);
    }
};

} // namespace

ConfigurePerGameAddons::ConfigurePerGameAddons(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigurePerGameAddons>()}, system{system_} {
    ui->setupUi(this);

    layout = new QVBoxLayout;
    tree_view = new QTreeView;
    auto* model = new AddonModel(tree_view);
    model->on_drop = [this](const QMimeData* mime, const QModelIndex& drop_parent, int row) {
        return OnDrop(mime, drop_parent, row);
    };
    item_model = model;
    tree_view->setModel(item_model);
    tree_view->setAlternatingRowColors(true);
    tree_view->setSelectionMode(QHeaderView::ExtendedSelection);
    tree_view->setSelectionBehavior(QHeaderView::SelectRows);
    tree_view->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    // Manual order and column sorting are mutually exclusive; the first build is alphabetical.
    tree_view->setSortingEnabled(false);
    tree_view->setEditTriggers(QHeaderView::DoubleClicked | QHeaderView::EditKeyPressed);
    tree_view->setExpandsOnDoubleClick(false);
    tree_view->setDragDropOverwriteMode(false);
    tree_view->setDropIndicatorShown(true);
    tree_view->setDragDropMode(QHeaderView::NoDragDrop);
    tree_view->setUniformRowHeights(true);
    tree_view->setContextMenuPolicy(Qt::CustomContextMenu);

    item_model->insertColumns(0, 2);
    item_model->setHeaderData(0, Qt::Horizontal, tr("Patch Name"));
    item_model->setHeaderData(1, Qt::Horizontal, tr("Version"));

    tree_view->header()->setStretchLastSection(false);
    tree_view->header()->setSectionResizeMode(0, QHeaderView::ResizeMode::Stretch);
    tree_view->header()->setMinimumSectionSize(150);

    // We must register all custom types with the Qt Automoc system so that we are able to use it
    // with signals/slots. In this case, QList falls under the umbrella of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tree_view);

    ui->scrollArea->setLayout(layout);

    ui->scrollArea->setEnabled(!system.IsPoweredOn());
    ui->create_category->setEnabled(!system.IsPoweredOn());

    connect(item_model, &QStandardItemModel::itemChanged, this,
            &ConfigurePerGameAddons::OnItemChanged);
    connect(item_model, &QStandardItemModel::itemChanged,
            [] { UISettings::values.is_game_list_reload_pending.exchange(true); });

    connect(ui->folder, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModFolder);
    connect(ui->zip, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModZip);
    connect(ui->create_category, &QAbstractButton::clicked, this,
            &ConfigurePerGameAddons::CreateModCategory);

    connect(tree_view, &QTreeView::customContextMenuRequested, this,
            &ConfigurePerGameAddons::showContextMenu);
}

ConfigurePerGameAddons::~ConfigurePerGameAddons() = default;

void ConfigurePerGameAddons::OnItemChanged(QStandardItem* item) {
    if (rebuilding || item == nullptr) {
        return;
    }

    const int kind = item->data(ENTRY_KIND).toInt();

    if (kind == CategoryRow) {
        const auto stored = item->data(STORED_NAME).toString();
        if (!stored.isEmpty() && item->text() != stored) {
            // Inline rename, either from a double click or from F2.
            const auto name = item->text().trimmed();
            if (name.isEmpty() || CategoryNameExists(name, stored)) {
                QtCommon::Frontend::Critical(
                    tr("Invalid Category Name"),
                    tr("Category names must be non-empty and unique."));
                rebuilding = true;
                item->setText(stored);
                rebuilding = false;
                return;
            }
            for (auto& category : categories.categories) {
                if (category.name == stored.toStdString()) {
                    category.name = name.toStdString();
                    break;
                }
            }
            rebuilding = true;
            item->setText(name);
            item->setData(name, STORED_NAME);
            rebuilding = false;
            SaveCategories();
            return;
        }
        ApplyCategoryCheckState(item);
        return;
    }

    if (kind == ModRow) {
        if (auto* parent = item->parent(); parent != nullptr &&
            parent->data(ENTRY_KIND).toInt() == CategoryRow) {
            RefreshCategoryCheckState(parent);
        }
    }

    if (update_items.size() > 1 && item->checkState() == Qt::Checked) {
        auto it = std::find(update_items.begin(), update_items.end(), item);
        if (it != update_items.end()) {
            for (auto* update_item : update_items) {
                if (update_item != item && update_item->checkState() == Qt::Checked) {
                    disconnect(item_model, &QStandardItemModel::itemChanged, this,
                               &ConfigurePerGameAddons::OnItemChanged);
                    update_item->setCheckState(Qt::Unchecked);
                    connect(item_model, &QStandardItemModel::itemChanged, this,
                            &ConfigurePerGameAddons::OnItemChanged);
                }
            }
        }
    }
}

void ConfigurePerGameAddons::ApplyCategoryCheckState(QStandardItem* category) {
    const auto state = category->checkState();
    if (state == Qt::PartiallyChecked) {
        // Partially checked is only ever set from the children's states.
        return;
    }

    const bool was_rebuilding = rebuilding;
    rebuilding = true;
    for (int row = 0; row < category->rowCount(); ++row) {
        auto* child = category->child(row, 0);
        if (child != nullptr && child->data(ENTRY_KIND).toInt() == ModRow) {
            child->setCheckState(state);
        }
    }
    rebuilding = was_rebuilding;
}

void ConfigurePerGameAddons::RefreshCategoryCheckState(QStandardItem* category) {
    int checked = 0;
    int total = 0;
    for (int row = 0; row < category->rowCount(); ++row) {
        const auto* child = category->child(row, 0);
        if (child == nullptr || child->data(ENTRY_KIND).toInt() != ModRow) {
            continue;
        }
        ++total;
        if (child->checkState() == Qt::Checked) {
            ++checked;
        }
    }

    const auto state = total == 0 || checked == 0
                           ? Qt::Unchecked
                           : (checked == total ? Qt::Checked : Qt::PartiallyChecked);
    const bool was_rebuilding = rebuilding;
    rebuilding = true;
    category->setCheckState(state);
    rebuilding = was_rebuilding;
}

void ConfigurePerGameAddons::ApplyConfiguration() {
    std::vector<std::string> disabled_addons;

    for (const auto& item : list_items) {
        const auto disabled = item.front()->checkState() == Qt::Unchecked;
        if (disabled) {
            QVariant userData = item.front()->data(Qt::UserRole);
            if (userData.isValid() && userData.canConvert<quint32>() &&
                item.front()->text() == QStringLiteral("Update")) {
                quint32 numeric_version = userData.toUInt();
                disabled_addons.push_back(fmt::format("Update@{}", numeric_version));
            } else {
                disabled_addons.push_back(item.front()->text().toStdString());
            }
        }
    }

    auto current = Settings::values.disabled_addons[title_id];
    std::sort(disabled_addons.begin(), disabled_addons.end());
    std::sort(current.begin(), current.end());
    if (disabled_addons != current) {
        Common::FS::RemoveFile(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                               "game_list" / fmt::format("{:016X}.pv.txt", title_id));
    }

    Settings::values.disabled_addons[title_id] = disabled_addons;
}

void ConfigurePerGameAddons::LoadFromFile(FileSys::VirtualFile file_) {
    SetFile(std::move(file_));
    LoadConfiguration();
}

void ConfigurePerGameAddons::SetFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
}

void ConfigurePerGameAddons::SetTitleId(u64 id) {
    this->title_id = id;
    category_store = std::make_unique<FrontendCommon::ModCategoryStore>(id);
}

void ConfigurePerGameAddons::InstallMods(const QStringList& mods) {
    QStringList failed;
    for (const auto& mod : mods) {
        if (FrontendCommon::InstallMod(mod.toStdString(), title_id, true) ==
            FrontendCommon::Failed) {
            failed << QFileInfo(mod).baseName();
        }
    }

    if (failed.empty()) {
        QtCommon::Frontend::Information(tr("Mod Install Succeeded"),
                                        tr("Successfully installed all mods."));

        item_model->removeRows(0, item_model->rowCount());
        list_items.clear();
        LoadConfiguration();

        UISettings::values.is_game_list_reload_pending.exchange(true);
    } else {
        QtCommon::Frontend::Critical(
            tr("Mod Install Failed"),
            tr("Failed to install the following mods:\n\t%1\nCheck the log for details.")
                .arg(failed.join(QStringLiteral("\n\t"))));
    }
}

void ConfigurePerGameAddons::InstallModPath(const QString& path, const QString& fallbackName) {
    const auto mods = QtCommon::Mod::GetModFolders(path, fallbackName);

    if (mods.size() > 1) {
        ModSelectDialog* dialog = new ModSelectDialog(mods, this);
        connect(dialog, &ModSelectDialog::modsSelected, this, &ConfigurePerGameAddons::InstallMods);
        dialog->show();
    } else if (!mods.empty()) {
        InstallMods(mods);
    }
}

void ConfigurePerGameAddons::InstallModFolder() {
    const auto path = QtCommon::Frontend::GetExistingDirectory(
        tr("Mod Folder"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (path.isEmpty()) {
        return;
    }

    InstallModPath(path);
}

void ConfigurePerGameAddons::InstallModZip() {
    // TODO(crueter): use GetOpenFileName to allow select multiple ZIPs
    const auto path = QtCommon::Frontend::GetOpenFileName(
        tr("Zipped Mod Location"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Zipped Archives (*.zip)"));
    if (path.isEmpty()) {
        return;
    }

    const QString extracted = QtCommon::Mod::ExtractMod(path);
    if (!extracted.isEmpty())
        InstallModPath(extracted, QFileInfo(path).baseName());
}

void ConfigurePerGameAddons::AddonDeleteRequested(QList<QModelIndex> selected) {
    QList<QModelIndex> filtered;
    for (const QModelIndex& index : selected) {
        if (!index.data(PATCH_LOCATION).toString().isEmpty())
            filtered << index;
    }

    if (filtered.empty()) {
        QtCommon::Frontend::Critical(tr("Invalid Selection"),
                                     tr("Only mods, cheats, and patches can be deleted.\nTo delete "
                                        "NAND-installed updates, right-click the game in the game "
                                        "list and click Remove -> Remove Installed Update."));
        return;
    }

    const auto header = tr("You are about to delete the following installed mods:\n");
    QString selected_str;
    for (const QModelIndex& index : filtered) {
        selected_str = selected_str % index.data().toString() % QStringLiteral("\n");
    }

    const auto footer = tr("\nOnce deleted, these can NOT be recovered. Are you 100% sure "
                           "you want to delete them?");

    QString caption = header % selected_str % footer;

    auto choice = QtCommon::Frontend::Warning(tr("Delete add-on(s)?"), caption,
                                              QtCommon::Frontend::StandardButton::Yes |
                                                  QtCommon::Frontend::StandardButton::No);

    if (choice == QtCommon::Frontend::StandardButton::No)
        return;

    QStringList deleted;
    for (const QModelIndex& index : filtered) {
        std::filesystem::remove_all(index.data(PATCH_LOCATION).toString().toStdString());
        deleted << index.data().toString();
    }

    // Explicitly deleted mods must not linger as "not found" placeholders.
    ForgetMods(deleted);
    SaveCategories();

    QtCommon::Frontend::Information(tr("Successfully deleted"),
                                    tr("Successfully deleted all selected mods."));

    item_model->removeRows(0, item_model->rowCount());
    list_items.clear();
    LoadConfiguration();

    UISettings::values.is_game_list_reload_pending.exchange(true);
}

void ConfigurePerGameAddons::showContextMenu(const QPoint& pos) {
    QModelIndex index = tree_view->indexAt(pos);
    if (index.isValid()) {
        index = index.siblingAtColumn(0);
    }

    if (index.isValid()) {
        const int kind = index.data(ENTRY_KIND).toInt();

        if (kind == CategoryRow) {
            auto* category = item_model->itemFromIndex(index);
            const bool is_uncategorized = index.data(STORED_NAME).toString().isEmpty();

            QMenu menu(this);
            if (!is_uncategorized) {
                QAction* rename = menu.addAction(tr("&Rename Category"));
                connect(rename, &QAction::triggered, this,
                        [this, category]() { RenameCategory(category); });

                QAction* remove = menu.addAction(tr("&Delete Category"));
                connect(remove, &QAction::triggered, this,
                        [this, category]() { DeleteCategory(category); });
            }

            bool has_deletable_mods = false;
            for (int row = 0; row < category->rowCount() && !has_deletable_mods; ++row) {
                const auto* child = category->child(row, 0);
                has_deletable_mods =
                    child != nullptr && !child->data(PATCH_LOCATION).toString().isEmpty();
            }

            QAction* delete_mods = menu.addAction(tr("Delete All Mods in &Category"));
            delete_mods->setEnabled(has_deletable_mods);
            connect(delete_mods, &QAction::triggered, this,
                    [this, category]() { DeleteAllModsInCategory(category); });

            menu.exec(tree_view->viewport()->mapToGlobal(pos));
            return;
        }

        if (kind == MissingModRow) {
            QMenu menu(this);
            QAction* remove = menu.addAction(tr("&Remove from Category"));
            auto* item = item_model->itemFromIndex(index);
            connect(remove, &QAction::triggered, this,
                    [this, item]() { RemoveMissingMod(item); });
            menu.exec(tree_view->viewport()->mapToGlobal(pos));
            return;
        }
    }

    auto selected = tree_view->selectionModel()->selectedRows();
    if (index.isValid() && selected.empty()) {
        selected << index;
    }

    if (selected.empty())
        return;

    QMenu menu(this);

    QAction* remove = menu.addAction(tr("&Delete"));
    connect(remove, &QAction::triggered, this,
            [this, selected]() { AddonDeleteRequested(selected); });

    if (selected.length() == 1) {
        auto loc = selected.at(0).data(PATCH_LOCATION).toString();
        if (QFileInfo::exists(loc)) {
            QAction* open = menu.addAction(tr("&Open in File Manager"));
            connect(open, &QAction::triggered, this,
                    [selected, loc]() { QDesktopServices::openUrl(QUrl::fromLocalFile(loc)); });
        }
    }

    menu.exec(tree_view->viewport()->mapToGlobal(pos));
}

bool ConfigurePerGameAddons::CategoryNameExists(const QString& name,
                                                const QString& renamed_from) const {
    if (name.compare(tr("Uncategorized"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return std::ranges::any_of(categories.categories, [&](const auto& category) {
        // The category being renamed does not collide with itself, which also allows fixing
        // capitalisation.
        if (!renamed_from.isEmpty() && category.name == renamed_from.toStdString()) {
            return false;
        }
        return name.compare(QString::fromStdString(category.name), Qt::CaseInsensitive) == 0;
    });
}

void ConfigurePerGameAddons::SaveCategories() {
    if (category_store != nullptr && !category_store->Save(categories)) {
        LOG_ERROR(Frontend, "Could not save the mod categories for title {:016X}", title_id);
    }
}

void ConfigurePerGameAddons::ForgetMods(const QStringList& names) {
    const auto erase = [&names](std::vector<std::string>& mods) {
        std::erase_if(mods, [&](const std::string& mod) {
            return names.contains(QString::fromStdString(mod));
        });
    };
    for (auto& category : categories.categories) {
        erase(category.mods);
    }
    erase(categories.uncategorized);
}

void ConfigurePerGameAddons::CreateModCategory() {
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Create Mod Category"), tr("Category name:"),
                                            QLineEdit::Normal, tr("Category"), &accepted)
                          .trimmed();
    if (!accepted) {
        return;
    }
    if (name.isEmpty() || CategoryNameExists(name)) {
        QtCommon::Frontend::Critical(tr("Invalid Category Name"),
                                     tr("Category names must be non-empty and unique."));
        return;
    }

    categories.categories.push_back({name.toStdString(), {}});
    SaveCategories();
    LoadConfiguration();
}

void ConfigurePerGameAddons::RenameCategory(QStandardItem* category) {
    const auto stored = category->data(STORED_NAME).toString();
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Rename Category"), tr("Category name:"),
                                            QLineEdit::Normal, stored, &accepted)
                          .trimmed();
    if (!accepted || name == stored) {
        return;
    }
    if (name.isEmpty() || CategoryNameExists(name, stored)) {
        QtCommon::Frontend::Critical(tr("Invalid Category Name"),
                                     tr("Category names must be non-empty and unique."));
        return;
    }

    for (auto& entry : categories.categories) {
        if (entry.name == stored.toStdString()) {
            entry.name = name.toStdString();
            break;
        }
    }

    // Keep the group open across the reload, since expansion is tracked by name.
    if (expanded_groups.remove(stored) > 0) {
        expanded_groups.insert(name);
    }

    SaveCategories();
    LoadConfiguration();
}

void ConfigurePerGameAddons::DeleteCategory(QStandardItem* category) {
    const auto stored = category->data(STORED_NAME).toString().toStdString();
    const auto it = std::ranges::find_if(categories.categories, [&](const auto& entry) {
        return entry.name == stored;
    });
    if (it == categories.categories.end()) {
        return;
    }

    // Deleting a category never deletes mods: they fall back to Uncategorized.
    categories.uncategorized.insert(categories.uncategorized.end(), it->mods.begin(),
                                    it->mods.end());
    categories.categories.erase(it);
    SaveCategories();
    LoadConfiguration();
}

void ConfigurePerGameAddons::DeleteAllModsInCategory(QStandardItem* category) {
    QList<QModelIndex> selected;
    for (int row = 0; row < category->rowCount(); ++row) {
        const auto* child = category->child(row, 0);
        if (child != nullptr && !child->data(PATCH_LOCATION).toString().isEmpty()) {
            selected << child->index();
        }
    }
    if (selected.empty()) {
        // Nothing on disk to delete (only missing mods); the delete confirmation would
        // otherwise report an invalid selection.
        return;
    }
    AddonDeleteRequested(selected);
}

void ConfigurePerGameAddons::RemoveMissingMod(QStandardItem* item) {
    ForgetMods({item->text()});
    SaveCategories();
    LoadConfiguration();
}

bool ConfigurePerGameAddons::OnDrop(const QMimeData* mime, const QModelIndex& parent, int row) {
    if (categories.categories.empty() || system.IsPoweredOn() || mime == nullptr) {
        return false;
    }

    const auto lines = QString::fromUtf8(mime->data(MimeType)).split(QLatin1Char('\n'));
    if (lines.size() < 2) {
        return false;
    }
    // Drops can land on either column of a row; resolve the row through its first column.
    const QModelIndex drop_parent = parent.isValid() ? parent.siblingAtColumn(0) : parent;

    const bool is_category = lines.front() == QStringLiteral("C");
    const auto names = lines.mid(1);

    if (is_category) {
        if (drop_parent.isValid()) {
            // The list is flat: a category can only be reordered, never nested.
            return false;
        }

        std::vector<std::size_t> moved_indices;
        for (const auto& name : names) {
            const auto it = std::ranges::find_if(categories.categories, [&](const auto& entry) {
                return entry.name == name.toStdString();
            });
            if (it == categories.categories.end()) {
                continue;
            }
            const auto position =
                static_cast<std::size_t>(std::distance(categories.categories.begin(), it));
            if (std::ranges::find(moved_indices, position) == moved_indices.end()) {
                moved_indices.push_back(position);
            }
        }
        if (moved_indices.empty()) {
            return false;
        }
        std::sort(moved_indices.begin(), moved_indices.end());

        // Count how many categories sit above the drop position.
        int to = static_cast<int>(categories.categories.size());
        if (row >= 0) {
            to = 0;
            for (int r = 0; r < row; ++r) {
                const auto index = item_model->index(r, 0);
                if (index.isValid() && index.data(ENTRY_KIND).toInt() == CategoryRow &&
                    !index.data(STORED_NAME).toString().isEmpty()) {
                    ++to;
                }
            }
        }

        const auto removed_before = static_cast<int>(
            std::ranges::count_if(moved_indices, [to](std::size_t position) {
                return static_cast<int>(position) < to;
            }));

        std::vector<FrontendCommon::ModCategory> moved;
        moved.reserve(moved_indices.size());
        for (const auto position : moved_indices) {
            moved.push_back(categories.categories[position]);
        }
        for (auto it = moved_indices.rbegin(); it != moved_indices.rend(); ++it) {
            categories.categories.erase(categories.categories.begin() + *it);
        }

        to = std::clamp(to - removed_before, 0, static_cast<int>(categories.categories.size()));
        categories.categories.insert(categories.categories.begin() + to, moved.begin(),
                                     moved.end());
        SaveCategories();
        LoadConfiguration();
        return true;
    }

    QStandardItem* destination = nullptr;
    int insert_at = -1;
    if (drop_parent.isValid()) {
        const int kind = drop_parent.data(ENTRY_KIND).toInt();
        if (kind == CategoryRow) {
            destination = item_model->itemFromIndex(drop_parent);
            insert_at = row;
        } else if (kind == ModRow || kind == MissingModRow) {
            destination = item_model->itemFromIndex(drop_parent.parent());
            insert_at = drop_parent.row();
        } else {
            return false;
        }
    } else {
        // Dropped below the list: back to Uncategorized, at the end.
        for (int r = 0; r < item_model->rowCount(); ++r) {
            const auto index = item_model->index(r, 0);
            if (index.data(ENTRY_KIND).toInt() == CategoryRow &&
                index.data(STORED_NAME).toString().isEmpty()) {
                destination = item_model->itemFromIndex(index);
                break;
            }
        }
    }
    if (destination == nullptr || destination->data(ENTRY_KIND).toInt() != CategoryRow) {
        return false;
    }

    const auto destination_name = destination->data(STORED_NAME).toString().toStdString();
    std::vector<std::string>* target = &categories.uncategorized;
    if (!destination_name.empty()) {
        const auto it = std::ranges::find_if(categories.categories, [&](const auto& entry) {
            return entry.name == destination_name;
        });
        if (it == categories.categories.end()) {
            return false;
        }
        target = &it->mods;
    }

    // Resolve the insert position before removing anything, so a drag inside one group keeps its
    // place relative to the rows that stay behind.
    const std::vector<std::string> original = *target;
    int position = static_cast<int>(original.size());
    if (insert_at >= 0) {
        position = 0;
        for (int i = 0; i < insert_at && i < static_cast<int>(original.size()); ++i) {
            if (!names.contains(QString::fromStdString(original[i]))) {
                ++position;
            }
        }
    }

    ForgetMods(names);
    position = std::clamp(position, 0, static_cast<int>(target->size()));
    for (const auto& name : names) {
        target->insert(target->begin() + position, name.toStdString());
        ++position;
    }

    SaveCategories();
    LoadConfiguration();
    return true;
}

void ConfigurePerGameAddons::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigurePerGameAddons::RetranslateUI() {
    ui->retranslateUi(this);
    item_model->setHeaderData(0, Qt::Horizontal, tr("Patch Name"));
    item_model->setHeaderData(1, Qt::Horizontal, tr("Version"));

    // Only the fixed label needs a new translation. Rebuilding the list here would silently
    // discard checkbox changes that have not been applied yet.
    const bool was_rebuilding = rebuilding;
    rebuilding = true;
    for (int row = 0; row < item_model->rowCount(); ++row) {
        const auto index = item_model->index(row, 0);
        if (index.data(ENTRY_KIND).toInt() == CategoryRow &&
            index.data(STORED_NAME).toString().isEmpty()) {
            item_model->itemFromIndex(index)->setText(tr("Uncategorized"));
        }
    }
    rebuilding = was_rebuilding;
}

void ConfigurePerGameAddons::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    // Keep which groups the user had collapsed across the rebuild.
    bool had_groups = false;
    for (int row = 0; row < item_model->rowCount(); ++row) {
        const auto index = item_model->index(row, 0);
        if (index.data(ENTRY_KIND).toInt() != CategoryRow) {
            continue;
        }
        had_groups = true;
        const auto key = index.data(STORED_NAME).toString();
        if (tree_view->isExpanded(index)) {
            expanded_groups.insert(key);
        } else {
            expanded_groups.remove(key);
        }
    }
    if (had_groups) {
        expansion_restored = true;
    }

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto loader = Loader::GetLoader(system, file);

    FileSys::VirtualFile update_raw;
    loader->ReadUpdateRaw(update_raw);

    const auto& disabled = Settings::values.disabled_addons[title_id];

    rebuilding = true;
    update_items.clear();
    list_items.clear();
    item_model->removeRows(0, item_model->rowCount());

    if (category_store != nullptr) {
        categories = category_store->Load();
    }

    std::vector<FileSys::Patch> patches = pm.GetPatches(update_raw);

    bool has_enabled_update = false;

    const auto make_row = [&](const FileSys::Patch& patch) {
        const auto name = QString::fromStdString(patch.name);
        const bool groupable = IsGroupableMod(patch);

        auto* const first_item = new QStandardItem;
        first_item->setText(name);
        first_item->setCheckable(true);
        first_item->setData(groupable ? ModRow : OtherRow, ENTRY_KIND);

        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (groupable) {
            flags |= Qt::ItemIsDragEnabled;
        }
        first_item->setFlags(flags);

        const bool is_external_update = patch.type == FileSys::PatchType::Update &&
                                        patch.source == FileSys::PatchSource::External &&
                                        patch.numeric_version != 0;

        const bool is_mod = patch.type == FileSys::PatchType::Mod;

        if (is_external_update) {
            first_item->setData(static_cast<quint32>(patch.numeric_version), NUMERIC_VERSION);
        } else if (is_mod) {
            first_item->setData(QString::fromStdString(patch.location), PATCH_LOCATION);
        }

        bool patch_disabled = false;
        if (is_external_update) {
            std::string disabled_key = fmt::format("Update@{}", patch.numeric_version);
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), disabled_key) != disabled.end();
        } else {
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), name.toStdString()) != disabled.end();
        }

        bool should_enable = !patch_disabled;

        if (patch.type == FileSys::PatchType::Update) {
            if (should_enable) {
                if (has_enabled_update) {
                    should_enable = false;
                } else {
                    has_enabled_update = true;
                }
            }
            update_items.push_back(first_item);
        }

        first_item->setCheckState(should_enable ? Qt::Checked : Qt::Unchecked);

        // The version cell keeps the row droppable but must not be editable: items are editable
        // by default, and the view now has double-click editing enabled for category renames.
        auto* const version_item = new QStandardItem{QString::fromStdString(patch.version)};
        Qt::ItemFlags version_flags =
            Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
        if (groupable) {
            version_flags |= Qt::ItemIsDragEnabled;
        }
        version_item->setFlags(version_flags);

        return QList<QStandardItem*>{first_item, version_item};
    };

    const auto by_name = [](const FileSys::Patch& left, const FileSys::Patch& right) {
        return left.name < right.name;
    };

    const bool grouped = !categories.categories.empty();

    if (!grouped) {
        // No categories yet: the same flat, alphabetical list as before.
        std::sort(patches.begin(), patches.end(), by_name);
        for (const auto& patch : patches) {
            auto row = make_row(patch);
            list_items.push_back(row);
            item_model->appendRow(row);
        }
    } else {
        std::vector<FileSys::Patch> others;
        std::vector<FileSys::Patch> mods;
        for (const auto& patch : patches) {
            (IsGroupableMod(patch) ? mods : others).push_back(patch);
        }
        std::sort(others.begin(), others.end(), by_name);
        for (const auto& patch : others) {
            auto row = make_row(patch);
            list_items.push_back(row);
            item_model->appendRow(row);
        }

        std::map<std::string, const FileSys::Patch*> mods_by_name;
        for (const auto& mod : mods) {
            mods_by_name[mod.name] = &mod;
        }

        std::set<std::string> assigned;
        const auto append_group = [&](const QString& display, const std::string& stored,
                                      const std::vector<std::string>& names) {
            auto* category = new QStandardItem(display);
            category->setData(CategoryRow, ENTRY_KIND);
            category->setData(QString::fromStdString(stored), STORED_NAME);
            category->setCheckable(true);

            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                                  Qt::ItemIsUserCheckable | Qt::ItemIsDropEnabled;
            if (!stored.empty()) {
                flags |= Qt::ItemIsDragEnabled | Qt::ItemIsEditable;
            }
            category->setFlags(flags);

            auto* const category_version = new QStandardItem;
            Qt::ItemFlags category_version_flags =
                Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
            if (!stored.empty()) {
                category_version_flags |= Qt::ItemIsDragEnabled;
            }
            category_version->setFlags(category_version_flags);

            item_model->appendRow(QList<QStandardItem*>{category, category_version});

            for (const auto& mod_name : names) {
                if (!assigned.insert(mod_name).second) {
                    continue;
                }
                const auto it = mods_by_name.find(mod_name);
                if (it != mods_by_name.end()) {
                    auto row = make_row(*it->second);
                    list_items.push_back(row);
                    category->appendRow(row);
                } else {
                    auto* missing = new QStandardItem(QString::fromStdString(mod_name));
                    missing->setData(MissingModRow, ENTRY_KIND);
                    missing->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                    auto font = missing->font();
                    font.setItalic(true);
                    missing->setFont(font);
                    missing->setForeground(tree_view->palette().color(QPalette::Disabled,
                                                                      QPalette::Text));
                    missing->setToolTip(tr("Mod folder not found"));
                    auto* const missing_version = new QStandardItem;
                    missing_version->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                                              Qt::ItemIsDropEnabled);
                    category->appendRow(QList<QStandardItem*>{missing, missing_version});
                }
            }
            return category;
        };

        for (const auto& category : categories.categories) {
            append_group(QString::fromStdString(category.name), category.name, category.mods);
        }

        auto uncategorized = categories.uncategorized;
        std::vector<std::string> leftovers;
        for (const auto& mod : mods) {
            const bool known = assigned.contains(mod.name) ||
                               std::ranges::find(categories.uncategorized, mod.name) !=
                                   categories.uncategorized.end();
            if (!known) {
                leftovers.push_back(mod.name);
            }
        }
        std::sort(leftovers.begin(), leftovers.end());
        uncategorized.insert(uncategorized.end(), leftovers.begin(), leftovers.end());
        append_group(tr("Uncategorized"), std::string{}, uncategorized);

        for (int row = 0; row < item_model->rowCount(); ++row) {
            const auto index = item_model->index(row, 0);
            if (index.data(ENTRY_KIND).toInt() != CategoryRow) {
                continue;
            }
            const auto key = index.data(STORED_NAME).toString();
            tree_view->setExpanded(index, !expansion_restored || expanded_groups.contains(key));
            RefreshCategoryCheckState(item_model->itemFromIndex(index));
        }
    }

    rebuilding = false;

    const bool can_organize = grouped && !system.IsPoweredOn();
    tree_view->setDragEnabled(can_organize);
    tree_view->setAcceptDrops(can_organize);
    tree_view->setDragDropMode(can_organize ? QHeaderView::InternalMove : QHeaderView::NoDragDrop);

    tree_view->resizeColumnToContents(1);
}
