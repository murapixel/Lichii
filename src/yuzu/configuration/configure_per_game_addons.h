// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include "core/file_sys/vfs/vfs_types.h"
#include "frontend_common/mod_category_store.h"

namespace Core {
class System;
}

class QGraphicsScene;
class QMimeData;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QVBoxLayout;

namespace Ui {
class ConfigurePerGameAddons;
}

class ConfigurePerGameAddons : public QWidget {
    Q_OBJECT

public:
    enum PatchData {
        NUMERIC_VERSION = Qt::UserRole,
        PATCH_LOCATION,
        ENTRY_KIND,
        STORED_NAME,
    };

    /// What a row in the tree represents. Only ModRow entries can be put in a category.
    enum EntryKind {
        OtherRow,
        ModRow,
        CategoryRow,
        MissingModRow,
    };

    explicit ConfigurePerGameAddons(Core::System& system_, QWidget* parent = nullptr);
    ~ConfigurePerGameAddons() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();

    void LoadFromFile(FileSys::VirtualFile file_);
    void SetFile(FileSys::VirtualFile file_);

    void SetTitleId(u64 id);

public slots:
    void InstallMods(const QStringList& mods);
    void InstallModPath(const QString& path, const QString& fallbackName = {});

    void InstallModFolder();
    void InstallModZip();

    void AddonDeleteRequested(QList<QModelIndex> selected);

protected:
    void showContextMenu(const QPoint& pos);

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void LoadConfiguration();
    void OnItemChanged(QStandardItem* item);

    void CreateModCategory();
    void RenameCategory(QStandardItem* category);
    void DeleteCategory(QStandardItem* category);
    void DeleteAllModsInCategory(QStandardItem* category);
    void RemoveMissingMod(QStandardItem* item);

    bool OnDrop(const QMimeData* mime, const QModelIndex& parent, int row);
    void ApplyCategoryCheckState(QStandardItem* category);
    void RefreshCategoryCheckState(QStandardItem* category);

    bool CategoryNameExists(const QString& name, const QString& renamed_from = {}) const;
    void SaveCategories();
    void ForgetMods(const QStringList& names);

    std::unique_ptr<Ui::ConfigurePerGameAddons> ui;
    FileSys::VirtualFile file;
    u64 title_id;

    QVBoxLayout* layout;
    QTreeView* tree_view;
    QStandardItemModel* item_model;

    std::vector<QList<QStandardItem*>> list_items;
    std::vector<QStandardItem*> update_items;

    std::unique_ptr<FrontendCommon::ModCategoryStore> category_store;
    FrontendCommon::ModCategories categories;
    QSet<QString> expanded_groups;
    bool expansion_restored = false;
    bool rebuilding = false;

    Core::System& system;
};
