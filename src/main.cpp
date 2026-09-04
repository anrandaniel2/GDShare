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
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <filesystem>
#include <memory>
#include <optional>
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

// Whether we can safely hand this level's audio to GMD-API for bundling.
//
// This is purely a crash guard, NOT a policy check. GMD-API calls
// Zip::addFrom() on the song path unconditionally when includeSong is set,
// and addFrom() does readBinary() with no existence check -- so a missing,
// empty or unreadable song makes the zip writer fail with a bare
// "Unable to write entry data (code -3)" (MZ_DATA_ERROR) and takes the whole
// export down with it. Songs routinely aren't on disk: never downloaded, or
// cleared from the cache.
//
// Deliberately NOT enforced here: GMD-API's importer requires the song to be
// named "<number>.mp3" (verifySongFileName). That check exists only on the
// import path and has no bearing on whether a file can be written into a zip.
// Refusing to export a perfectly good song just because *this* mod couldn't
// re-import it later would be the tail wagging the dog -- the .gmd2 is a
// normal zip and any tool can read the audio out of it. So we bundle whatever
// the level actually has, including official songs.
static bool canIncludeSong(GJGameLevel* level) {
    if (!level) {
        return false;
    }

    auto name = std::string(level->getAudioFileName());
    if (name.empty()) {
        return false;
    }

    // Whatever GMD-API is going to hand to the zip writer.
    auto path = std::filesystem::path(name);
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
// Write a .gmd2 ourselves instead of going through ExportGmdFile.
//
// GMD-API builds gmd2 with file::Zip::create() -- the in-memory overload --
// and that path is broken in the SDK: Zip::Impl::intoMemory() opens the
// stream with MZ_OPEN_MODE_CREATE only, omitting MZ_OPEN_MODE_WRITE:
//
//     ret->m_mode = MZ_OPEN_MODE_CREATE;                        // intoMemory()
//     ...inFile(file, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_WRITE) // create(path)
//
// minizip's zlib stream only calls deflateInit2() under
// `if (mode & MZ_OPEN_MODE_WRITE)`, so the deflate stream is never
// initialised and every mz_zip_entry_write() fails. That surfaces as
// "Unable to write entry data (code -3)" (MZ_DATA_ERROR) on EVERY gmd2
// export, with or without a song -- which is why dropping the song didn't
// help. The file-based Zip::create(path) sets both flags and works fine,
// so we use that and write the same entries GMD-API would.
static std::optional<std::string> writeGmd2(
    GJGameLevel* level, std::filesystem::path const& to, bool includeSong
) {
    auto zipRes = file::Zip::create(to);
    if (!zipRes) {
        return zipRes.unwrapErr();
    }
    auto zip = std::move(zipRes).unwrap();

    auto dict = std::make_unique<DS_Dictionary>();
    level->encodeWithCoder(dict.get());
    auto data = std::string(dict->saveRootSubDictToString());

    auto json = matjson::Value();
    if (includeSong) {
        auto songPath = std::filesystem::path(std::string(level->getAudioFileName()));
        json["song-file"] = songPath.filename().string();
        json["song-is-custom"] = level->m_songID;
        if (auto res = zip.addFrom(songPath); !res) {
            // Song is optional -- fall back to a songless gmd2 rather than
            // losing the export entirely.
            return writeGmd2(level, to, false);
        }
    }
    if (auto res = zip.add("level.meta", json.dump()); !res) {
        return res.unwrapErr();
    }
    if (auto res = zip.add("level.data", data); !res) {
        return res.unwrapErr();
    }
    return std::nullopt;
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
            if (type == GmdFileType::Gmd2) {
                err = writeGmd2(level, *path, canIncludeSong(level));
            }
            else {
                // gmd/lvl are plain writes and don't touch the zip code.
                err = ExportGmdFile::from(level)
                    .setType(type)
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
                err = writeGmd2(level, to, canIncludeSong(level));
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
                    // GMD-API rejects any bundled song not named "<number>.mp3"
                    // and fails the entire import over it. That's a fine rule
                    // for deciding whether to unpack the audio, but a terrible
                    // reason to refuse the level, so retry without the song.
                    if (!res) {
                        res = ImportGmdFile::from(path)
                            .inferType()
                            .setImportSong(false)
                            .intoLevel();
                    }
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

