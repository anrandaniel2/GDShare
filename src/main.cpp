#include <Geode/Loader.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/IDManager.hpp>
#include <Geode/modify/LevelListLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/async.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

using namespace geode::prelude;
using namespace gmd;

static auto IMPORT_PICK_OPTIONS = file::FilePickOptions {
    std::nullopt,
    {
        {
            "GD Level Files",
            { "*.gmd", "*.gmdl", "*.gmd2", "*.lvl" } // importing gmd2 and lvl files work (also pls thank me (fijiaura) in the changelog)
        }
    }
};

static auto EXPORT_FOLDER_OPTIONS = file::FilePickOptions {
    std::nullopt,
    {
        {
            "Folders",
            { "" }
        }
    }
};

// ---------------------------------------------------------------------------
// gmd2 + song support
// ---------------------------------------------------------------------------

// Format used when the typed filename has no recognisable extension.
// gmd2 is the only format that can carry the song file.
static constexpr auto PREFERRED_LEVEL_TYPE = GmdFileType::Gmd2;

// Pick the export format from whatever extension the user actually typed, so
// naming the file .gmd still produces a real .gmd. Falls back to gmd2.
static GmdFileType typeFromPath(std::filesystem::path const& path) {
    auto ext = path.extension().string();
    if (ext.size()) {
        ext = ext.substr(1);
        if (auto type = gmdTypeFromString(ext.c_str())) {
            return type.value();
        }
    }
    return PREFERRED_LEVEL_TYPE;
}

// GMD-API refuses to import a gmd2 whose song file isn't named "<number>.mp3"
// (verifySongFileName), and that failure aborts the whole import. Official
// in-game songs are named things like "BackOnTrack.mp3", so only bundle the
// audio when it's a custom/Newgrounds song, which is always numeric.
//
// We also have to confirm the file is actually on disk and non-empty before
// asking GMD-API to bundle it. It calls Zip::addFrom() unconditionally, and a
// missing/unreadable/empty song makes the zip writer fail with a bare
// "Unable to write entry data (code -3)" (MZ_DATA_ERROR), which aborts the
// entire export instead of just dropping the audio. This happens routinely:
// the song may have never been downloaded, or been cleared from the cache.
static bool canIncludeSong(GJGameLevel* level) {
    if (!level || level->m_songID == 0) {
        return false;
    }

    // Whatever GMD-API is going to hand to the zip writer.
    auto path = std::filesystem::path(std::string(level->getAudioFileName()));
    if (path.empty()) {
        return false;
    }

    // The name has to survive verifySongFileName() on the way back in,
    // otherwise we'd write a gmd2 that can never be imported again.
    auto name = path.filename().string();
    if (!name.ends_with(".mp3")) {
        return false;
    }
    auto stem = name.substr(0, name.size() - 4);
    if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
        return std::isdigit(c);
    })) {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }
    // An empty file would deflate to nothing and trip the same -3 error.
    return std::filesystem::file_size(path, ec) > 0 && !ec;
}

template <class L>
static arc::Future<file::PickResult> promptExportLevel(L* level) {
    auto opts = IMPORT_PICK_OPTIONS;
    if constexpr (std::is_same_v<L, GJLevelList>) {
        opts.defaultPath = std::string(level->m_listName) + ".gmdl";
    }
    else {
        // default to gmd2 so the song comes along
        opts.defaultPath = std::string(level->m_levelName) + ".gmd2";
    }
    return file::pick(file::PickMode::SaveFile, opts);
}
template <class L>
static void onExportFilePick(L* level, file::PickResult result) {
    if (result.isOk()) {
        auto path = std::move(result).unwrap();
        if (!path) return;
        std::optional<std::string> err;
        if constexpr (std::is_same_v<L, GJLevelList>) {
            err = exportListAsGmd(level, *path).err();
        }
        else {
            auto type = typeFromPath(*path);
            auto withSong = type == GmdFileType::Gmd2 && canIncludeSong(level);
            err = ExportGmdFile::from(level)
                .setType(type)
                .setIncludeSong(withSong)
                .intoFile(*path)
                .err();
            // Never let a bad song file cost the user their level export.
            if (err && withSong) {
                err = ExportGmdFile::from(level)
                    .setType(type)
                    .setIncludeSong(false)
                    .intoFile(*path)
                    .err();
            }
        }
        if (!err) {
            createQuickPopup(
                "Exported",
                (std::is_same_v<L, GJLevelList> ?
                    "Succesfully exported list" :
                    "Succesfully exported level"
                ),
                "OK", "Open File",
                [path](auto, bool btn2) {
                    if (btn2) file::openFolder(*path);
                }
            );
        }
        else {
            FLAlertLayer::create(
                "Error",
                "Unable to export: " + err.value(),
                "OK"
            )->show();
        }
    }
    else {
        FLAlertLayer::create("Error Exporting", result.unwrapErr(), "OK")->show();
    }
}

template <class L>
static void exportMany(std::vector<L*> levels, file::PickResult result) {
    if (result.isOk()) {
        auto optPath = std::move(result).unwrap();
        if (!optPath) return;
        auto path = std::move(*optPath);
        
        std::vector<std::string> errs;
        for (auto level : levels) {
            std::optional<std::string> err;
            if constexpr (std::is_same_v<L, GJLevelList>) {
                err = exportListAsGmd(level, path / (std::string(level->m_listName) + ".gmdl")).err();
            }
            else {
                auto to = path / (std::string(level->m_levelName) + ".gmd2");
                auto withSong = canIncludeSong(level);
                err = ExportGmdFile::from(level)
                    .setType(GmdFileType::Gmd2)
                    .setIncludeSong(withSong)
                    .intoFile(to)
                    .err();
                if (err && withSong) {
                    err = ExportGmdFile::from(level)
                        .setType(GmdFileType::Gmd2)
                        .setIncludeSong(false)
                        .intoFile(to)
                        .err();
                }
            }
            if (err) errs.push_back(*err);
        }

        size_t successCount = levels.size() - errs.size();
        auto successPortion = fmt::format(
            "Succesfully exported {} {}{}",
            successCount,
            (std::is_same_v<L, GJLevelList> ? "list" : "level"),
            successCount == 1 ? "" : "s"
        );
        if (errs.empty()) {
            createQuickPopup(
                "Exported",
                successPortion,
                "OK", "Open Folder",
                [path](auto, bool btn2) {
                    if (btn2) file::openFolder(path);
                }
            );
        }
        else {
            auto formattedErrsAndSuccess = fmt::format("Several errors occurred:\n- {}\n\n{}", fmt::join(errs, "\n- "), successPortion);
            createQuickPopup(
                "Exported with Errors",
                formattedErrsAndSuccess,
                "OK", "Open Folder",
                [path](auto, bool btn2) {
                    if (btn2) file::openFolder(path);
                }
            );
        }
    }
    else {
        FLAlertLayer::create("Error Exporting", result.unwrapErr(), "OK")->show();
    }
}

struct $modify(ExportMyLevelLayer, EditLevelLayer) {
    struct Fields {
        async::TaskHolder<file::PickResult> pickListener;
    };

    $override
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level))
            return false;
        
        auto menu = this->getChildByID("level-actions-menu");
        if (menu) {
            auto btn = CCMenuItemSpriteExtra::create(
                CircleButtonSprite::createWithSpriteFrameName(
                    "file.png"_spr, .8f,
                    CircleBaseColor::Green,
                    CircleBaseSize::MediumAlt
                ),
                this, menu_selector(ExportMyLevelLayer::onExport)
            );
            btn->setID("export-button"_spr);
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onExport(CCObject*) {
        m_fields->pickListener.spawn(
            promptExportLevel(m_level),
            [level = m_level](file::PickResult ev) {
                onExportFilePick(level, std::move(ev));
            }
        );
    }
};

struct $modify(ExportOnlineLevelLayer, LevelInfoLayer) {
    struct Fields {
        async::TaskHolder<file::PickResult> pickListener;
    };

    $override
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge))
            return false;
        
        auto menu = this->getChildByID("left-side-menu");
        if (menu) {
            auto btn = CCMenuItemSpriteExtra::create(
                CircleButtonSprite::createWithSpriteFrameName(
                    "file.png"_spr, .8f,
                    CircleBaseColor::Green,
                    CircleBaseSize::Medium
                ),
                this, menu_selector(ExportOnlineLevelLayer::onExport)
            );
            btn->setID("export-button"_spr);
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onExport(CCObject*) {
        m_fields->pickListener.spawn(
            promptExportLevel(m_level),
            [level = m_level](file::PickResult ev) {
                onExportFilePick(level, std::move(ev));
            }
        );
    }
};

struct $modify(ImportLayer, LevelBrowserLayer) {
    struct Fields {
        async::TaskHolder<file::PickManyResult> importListener;
        async::TaskHolder<file::PickResult> exportListener;
    };

    static void importFiles(std::vector<std::filesystem::path> const& paths) {
        for (auto const& path : paths) {
            switch (getGmdFileKind(path)) {
                case GmdFileKind::List: {
                    auto res = gmd::importGmdAsList(path);
                    if (res) {
                        LocalLevelManager::get()->m_localLists->insertObject(*res, 0);
                    }
                    else {
                        return FLAlertLayer::create("Error Importing", res.unwrapErr(), "OK")->show();
                    }
                } break;

                case GmdFileKind::Level: {
                    // setImportSong(true) is what actually unpacks the song out
                    // of a gmd2; gmd::importGmdAsLevel() leaves it off, which is
                    // why songs were being silently dropped.
                    auto res = ImportGmdFile::from(path)
                        .inferType()
                        .setImportSong(true)
                        .intoLevel();
                    if (res) {
                        LocalLevelManager::get()->m_localLevels->insertObject(*res, 0);
                    }
                    else {
                        return FLAlertLayer::create("Error Importing", res.unwrapErr(), "OK")->show();
                    }
                } break;

                case GmdFileKind::None: {
                    // todo: show popup to pick type
                    return FLAlertLayer::create(
                        "Error Importing",
                        fmt::format("Selected file '<cp>{}</c>' is not a GMD file!", path),
                        "OK"
                    )->show();
                } break;
            }
        }

        auto scene = CCScene::create();
        auto layer = LevelBrowserLayer::create(
            GJSearchObject::create(SearchType::MyLevels)
        );
        scene->addChild(layer);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.5f, scene));
    }

    void onImport(CCObject*) {
        m_fields->importListener.spawn(
            file::pickMany(IMPORT_PICK_OPTIONS),
            [](file::PickManyResult result) {
                if (result.isOk()) {
                    importFiles(std::move(result).unwrap());
                }
                else {
                    FLAlertLayer::create("Error Importing", result.unwrapErr(), "OK")->show();
                }
            }
        );
    }

    void onExport(CCObject*) {
        if (m_searchObject->m_searchType != SearchType::MyLevels) return;
        size_t count = 0;
        std::vector<GJGameLevel*> levelsForExporting = {};
        for (auto level : CCArrayExt<GJGameLevel*>(m_levels)) {
            if (!level->m_selected) continue;
            count += 1;
            levelsForExporting.push_back(level);
        }
        if (count < 1 || levelsForExporting.empty()) {
            FLAlertLayer::create("Nothing here...", "No levels selected for export.", "OK")->show();
        }
        else {
            m_fields->exportListener.spawn(
                file::pick(file::PickMode::OpenFolder, EXPORT_FOLDER_OPTIONS),
                [levelsForExporting](file::PickResult r) {
                    exportMany(levelsForExporting, std::move(r));
                }
            );
        }
    }

    $override
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search))
            return false;

        if (search->m_searchType == SearchType::MyLevels || search->m_searchType == SearchType::MyLists) {
            auto btnMenu = this->getChildByID("new-level-menu");

            auto importBtn = CCMenuItemSpriteExtra::create(
                CircleButtonSprite::createWithSpriteFrameName(
                    "file.png"_spr, .85f,
                    CircleBaseColor::Pink,
                    CircleBaseSize::Big
                ),
                this,
                menu_selector(ImportLayer::onImport)
            );
            importBtn->setID("import-level-button"_spr);

            // This one has an ID but no layout which is CRINGE
            if (search->m_searchType == SearchType::MyLists && search->m_searchIsOverlay) {
                btnMenu->addChildAtPosition(importBtn, Anchor::BottomLeft, ccp(0, 60), false);
            }
            else {
                btnMenu->addChild(importBtn);
                btnMenu->updateLayout();
            }

            auto otherBtnMenu = this->getChildByID("my-levels-menu");

            if (otherBtnMenu && search->m_searchType == SearchType::MyLevels && !search->m_searchIsOverlay) {
                auto exportBtn = CCMenuItemSpriteExtra::create(
                    CircleButtonSprite::createWithSpriteFrameName(
                        "file.png"_spr, .85f,
                        CircleBaseColor::Cyan,
                        CircleBaseSize::Big
                    ),
                    this,
                    menu_selector(ImportLayer::onExport)
                );
                exportBtn->setID("export-level-button"_spr);
                otherBtnMenu->addChild(exportBtn);
                otherBtnMenu->updateLayout();
            }
        }

        return true;
    }
};

struct $modify(ExportListLayer, LevelListLayer) {
    struct Fields {
        async::TaskHolder<file::PickResult> pickListener;
    };

    $override
    bool init(GJLevelList* level) {
        if (!LevelListLayer::init(level))
            return false;
        
        if (auto menu = this->getChildByID("left-side-menu")) {
            auto btn = CCMenuItemSpriteExtra::create(
                CircleButtonSprite::createWithSpriteFrameName(
                    "file.png"_spr, .8f,
                    CircleBaseColor::Green,
                    CircleBaseSize::Medium
                ),
                this, menu_selector(ExportListLayer::onExport)
            );
            btn->setID("export-button"_spr);
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onExport(CCObject*) {
        m_fields->pickListener.spawn(
            promptExportLevel(m_levelList),
            [list = m_levelList](file::PickResult ev) {
                onExportFilePick(list, std::move(ev));
            }
        );
    }
};

