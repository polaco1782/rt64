//
// RT64
//
// SPDX-License-Identifier: GPL-2.0-only
// Rice hashing is derived from GLideN64 and RiceVideo; see GLIDEN64-LICENSE.
// Modified 2026-10-16 for v64recomp: in-process API and capture bounds checks.

#include <filesystem>
#include <fstream>
#include <list>
#include <cstring>
#include <stdexcept>
#include "texture_hasher.h"

#include "../../contrib/xxHash/xxh3.h"
#ifdef RT64_TEXTURE_HASHER_LIBRARY
#include "../../common/rt64_load_types.h"
#include "../../common/rt64_replacement_database.h"
#else
#include "../../common/rt64_load_types.cpp"
#include "../../common/rt64_replacement_database.cpp"
#endif
#include "../../shared/rt64_f3d_defines.h"
#include "../../common/rt64_tmem_hasher.h"
#ifdef RT64_TEXTURE_HASHER_LIBRARY
#include "../../common/rt64_rice_hasher.h"
#else
#include "../../common/rt64_rice_hasher.cpp"
#endif

namespace {
static const std::string TileInfoExtension = ".tile.json";
static const std::string TMEMExtension = ".tmem";
static const std::string RiceInfoExtension = ".rice.json";
static const std::string RiceRdramExtension = ".rice.rdram";
static const std::string RicePaletteInfoExtension = ".rice.palette.json";
static const std::string RicePaletteRdramExtension = ".rice.palette.rdram";

bool endsWith(const std::string &str, const std::string &end) {
    if (str.length() >= end.length()) {
        return (str.compare(str.length() - end.length(), end.length(), end) == 0);
    }
    else {
        return false;
    }
}

bool loadTileInfoFromFile(const std::filesystem::path &directory, const std::string &hashName, RT64::LoadTile &tile, RT64::LoadTLUT &tlut, uint32_t &width, uint32_t &height) {
    try {
        std::filesystem::path infoPath = directory / (hashName + TileInfoExtension);
        if (!std::filesystem::exists(infoPath)) {
            return false;
        }

        std::ifstream infoStream(infoPath);
        json jroot;
        infoStream >> jroot;
        tile = jroot["tile"];
        tlut = jroot["tlut"];
        width = jroot["width"];
        height = jroot["height"];
        if (!infoStream.bad()) {
            return true;
        }
        else {
            std::string u8string = infoPath.u8string();
            fprintf(stderr, "Failed to read file at %s.", u8string.c_str());
            return false;
        }
    }
    catch (const nlohmann::detail::exception &e) {
        fprintf(stderr, "JSON parsing error: %s\n", e.what());
        return false;
    }
}

bool loadOperationInfoFromFile(const std::filesystem::path &directory, const std::string &hashName, RT64::LoadOperation &loadOp) {
    try {
        std::filesystem::path infoPath = directory / (hashName + RiceInfoExtension);
        if (!std::filesystem::exists(infoPath)) {
            return false;
        }

        std::ifstream infoStream(infoPath);
        json jroot;
        infoStream >> jroot;
        loadOp.texture = jroot["texture"];
        loadOp.tile = jroot["tile"];
        loadOp.type = jroot["type"];
        if (!infoStream.bad()) {
            return true;
        }
        else {
            std::string u8string = infoPath.u8string();
            fprintf(stderr, "Failed to read file at %s.", u8string.c_str());
            return false;
        }
    }
    catch (const nlohmann::detail::exception &e) {
        fprintf(stderr, "JSON parsing error: %s\n", e.what());
        return false;
    }
}

bool loadBytesFromFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes) {
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        if (size <= 0 || size > 8 * 1024 * 1024) {
            return false;
        }
        bytes.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read((char *)(bytes.data()), bytes.size());
        return bool(file);
    }
    else {
        return false;
    }
}

uint32_t toHashTLUT(RT64::LoadTLUT drawTLUT) {
    switch (drawTLUT) {
    case RT64::LoadTLUT::RGBA16:
        return G_TT_RGBA16;
    case RT64::LoadTLUT::IA16:
        return G_TT_IA16;
    default:
        return 0;
    }
}

struct RenameFile {
    const std::filesystem::path oldPath;
    const std::filesystem::path newPath;
};

void queueRenameFile(const std::filesystem::path &directory, const std::string &oldHashName, const std::string &newHashName, const std::string &suffix, std::list<RenameFile> &renameFileList) {
    // Skip if the file with the old name doesn't exist.
    const std::filesystem::path oldPath = directory / (oldHashName + suffix);
    if (!std::filesystem::exists(oldPath)) {
        return;
    }

    // Skip if the file with the new name already exists.
    const std::filesystem::path newPath = directory / (newHashName + suffix);
    if (std::filesystem::exists(newPath)) {
        return;
    }

    renameFileList.push_back({ oldPath, newPath });
}

void queueRenameFiles(const std::filesystem::path &directory, const std::string &oldHashName, const std::string &newHashName, std::list<RenameFile> &renameFileList) {
    if (oldHashName == newHashName) {
        return;
    }

    queueRenameFile(directory, oldHashName, newHashName, TileInfoExtension, renameFileList);
    queueRenameFile(directory, oldHashName, newHashName, TMEMExtension, renameFileList);
    queueRenameFile(directory, oldHashName, newHashName, RiceInfoExtension, renameFileList);
    queueRenameFile(directory, oldHashName, newHashName, RiceRdramExtension, renameFileList);
    queueRenameFile(directory, oldHashName, newHashName, RicePaletteInfoExtension, renameFileList);
    queueRenameFile(directory, oldHashName, newHashName, RicePaletteRdramExtension, renameFileList);
}

void executeRenameFiles(const std::list<RenameFile> &renameFileList) {
    for (const RenameFile &renameFile : renameFileList) {
        std::error_code ec;
        std::filesystem::rename(renameFile.oldPath, renameFile.newPath, ec);
        if (ec) {
            const std::string oldPathStr = renameFile.oldPath.u8string();
            const std::string newPathStr = renameFile.newPath.u8string();
            const std::string messageStr = ec.message();
            fprintf(stderr, "Failed to rename %s to %s (%s).\n", oldPathStr.c_str(), newPathStr.c_str(), messageStr.c_str());
        }
    }
}

void addRiceHash(const std::filesystem::path &directory, const std::string &hashNameWithSuffix, RT64::ReplacementDatabase &database, std::list<RenameFile> &renameFileList) {
    RT64::LoadTile drawTile = {};
    RT64::LoadTLUT drawTLUT;
    uint32_t drawWidth, drawHeight;
    if (!loadTileInfoFromFile(directory, hashNameWithSuffix, drawTile, drawTLUT, drawWidth, drawHeight)) {
        return;
    }

    RT64::LoadOperation loadOp = {};
    if (!loadOperationInfoFromFile(directory, hashNameWithSuffix, loadOp)) {
        return;
    }

    // Interior mip levels may have an unset load record in older captures.
    // There is no Rice source to hash; this is not a corrupt texture image.
    if (!loadOp.texture.width) {
        return;
    }

    if (!drawWidth || !drawHeight || drawTile.siz > 3 || loadOp.tile.siz > 3 ||
        loadOp.texture.siz > 3 || drawTile.masks > 15 || drawTile.maskt > 15 ||
        loadOp.tile.masks > 15 || loadOp.tile.maskt > 15) {
        fprintf(stderr, "Invalid texture parameters for %s.\n", hashNameWithSuffix.c_str());
        return;
    }

    std::vector<uint8_t> rdramBytes;
    if (!loadBytesFromFile(directory / (hashNameWithSuffix + RiceRdramExtension), rdramBytes)) {
        return;
    }

    std::vector<uint8_t> paletteBytes;
    if (drawTLUT != RT64::LoadTLUT::None &&
        !loadBytesFromFile(directory / (hashNameWithSuffix + RicePaletteRdramExtension), paletteBytes))
        return;
    const auto rice = RT64::calculateRiceHash(drawTile, loadOp, drawTLUT, rdramBytes, paletteBytes);
    if (rice.empty())
        return;
    // Extract the version from the hash name with the suffix.
    const std::string dotV = ".v";
    const size_t vPos = hashNameWithSuffix.find_first_of(dotV);
    bool redoHash = false;
    if (vPos != std::string::npos) {
        try {
            int versionFromName = std::stoi(hashNameWithSuffix.substr(vPos + dotV.size()));
            if (versionFromName != RT64::TMEMHasher::CurrentHashVersion) {
                redoHash = true;
            }
        }
        catch (const std::exception &e) {
            redoHash = true;
        }
    }
    else {
        redoHash = true;
    }

    // Redo the hash if necessary by loading from TMEM. If it requires raw TMEM, the hash is already correct.
    std::string currentHashName;
    if (redoHash && !RT64::TMEMHasher::requiresRawTMEM(drawTile, drawWidth, drawHeight)) {
        std::vector<uint8_t> tmemBytes;
        if (!loadBytesFromFile(directory / (hashNameWithSuffix + TMEMExtension), tmemBytes)) {
            return;
        }
        if (tmemBytes.size() != 4096) {
            return;
        }

        const uint64_t hash = RT64::TMEMHasher::hash(tmemBytes.data(), drawTile, drawWidth, drawHeight, toHashTLUT(drawTLUT), RT64::TMEMHasher::CurrentHashVersion);
        currentHashName = RT64::ReplacementDatabase::hashToString(hash);
    }
    else {
        currentHashName = hashNameWithSuffix.substr(0, vPos);
    }

    RT64::ReplacementTexture replacement = database.getReplacement(currentHashName);
    replacement.hashes.rt64 = currentHashName;
    replacement.hashes.rice = rice;

    database.addReplacement(replacement);

    if (redoHash) {
        const std::string newVersionSuffix = ".v" + std::to_string(RT64::TMEMHasher::CurrentHashVersion);
        queueRenameFiles(directory, hashNameWithSuffix, currentHashName + newVersionSuffix, renameFileList);
    }
}

void upgradeHash(const std::filesystem::path &directory, const std::string oldHashName, RT64::ReplacementDatabase &database, std::list<RenameFile> &renameFileList) {
    RT64::ReplacementTexture replacement = database.getReplacement(oldHashName);
    if (replacement.isEmpty()) {
        return;
    }
    
    RT64::LoadTile drawTile = {};
    RT64::LoadTLUT drawTLUT;
    uint32_t drawWidth, drawHeight;
    const std::string oldVersionSuffix = (database.config.hashVersion > 1) ? (".v" + std::to_string(database.config.hashVersion)) : "";
    if (!loadTileInfoFromFile(directory, oldHashName + oldVersionSuffix, drawTile, drawTLUT, drawWidth, drawHeight)) {
        return;
    }

    std::vector<uint8_t> tmemBytes;
    if (!loadBytesFromFile(directory / (oldHashName + oldVersionSuffix + TMEMExtension), tmemBytes)) {
        return;
    }
    
    const uint64_t hash = RT64::TMEMHasher::hash(tmemBytes.data(), drawTile, drawWidth, drawHeight, toHashTLUT(drawTLUT), RT64::TMEMHasher::CurrentHashVersion);
    const std::string newHashName = RT64::ReplacementDatabase::hashToString(hash);
    if (oldHashName != newHashName) {
        replacement.hashes.rt64 = newHashName;

        database.fixReplacement(oldHashName, replacement);
        fprintf(stdout, "Updated %s to %s in database.\n", oldHashName.c_str(), newHashName.c_str());

        const std::string newVersionSuffix = ".v" + std::to_string(RT64::TMEMHasher::CurrentHashVersion);
        queueRenameFiles(directory, oldHashName + oldVersionSuffix, newHashName + newVersionSuffix, renameFileList);
    }
}

void showHelp() {
    fprintf(stderr, 
        "texture_hasher <path> --rice\n"
        "\tGenerate Rice matches based on the dumped textures.\n\n"
        "texture_hasher <path> --upgrade\n"
        "\tUpgrade the database to the latest version. Will recalculate any hashes as necessary.\n\n"
    );
}

}

void RT64::generateRiceHashes(const std::filesystem::path &directory) {
    ReplacementDatabase database;
    database.config.autoPath = ReplacementAutoPath::Rice;
    const auto path = directory / "rt64.json";
    if (std::filesystem::exists(path)) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot read Rice capture database");
        json root;
        input >> root;
        database = root;
    }
    if (database.config.hashVersion != TMEMHasher::CurrentHashVersion)
        throw std::runtime_error("Rice capture database hash version does not match RT64");
    std::list<RenameFile> renames;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().u8string();
        if (entry.is_regular_file() && endsWith(name, RiceInfoExtension))
            addRiceHash(directory, name.substr(0, name.size() - RiceInfoExtension.size()), database, renames);
    }
    const auto temporary = directory / "rt64.json.new";
    std::ofstream output(temporary);
    output << json(database).dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("cannot write Rice capture database");
    executeRenameFiles(renames);
    std::filesystem::rename(temporary, path);
}

#ifndef RT64_TEXTURE_HASHER_LIBRARY
int main(int argc, char *argv[]) {
    if (argc < 3) {
        showHelp();
        return 1;
    }

    std::filesystem::path searchDirectory(argv[1]);
    if (!std::filesystem::is_directory(searchDirectory)) {
        std::string u8string = searchDirectory.u8string();
        fprintf(stderr, "The directory %s does not exist.", u8string.c_str());
        return 1;
    }

    enum class Mode {
        Rice,
        Upgrade
    };

    Mode mode = Mode::Rice;
    if (argc > 2) {
        std::string modeString = argv[2];
        if ((modeString == "--upgrade") || (modeString == "-u")) {
            fprintf(stdout, "Upgrading database to latest hash version.\n");
            mode = Mode::Upgrade;
        }
        else if ((modeString == "--rice") || (modeString == "-r")) {
            fprintf(stdout, "Generating Rice hashes for files in the directory.\n");
            mode = Mode::Rice;
        }
        else {
            fprintf(stderr, "Unrecognized argument %s.\n\n", modeString.c_str());
            showHelp();
            return 1;
        }
    }

    RT64::ReplacementDatabase database;
    database.config.autoPath = RT64::ReplacementAutoPath::Rice;

    std::filesystem::path databasePath = searchDirectory / "rt64.json";
    if (std::filesystem::exists(databasePath)) {
        std::ifstream databaseStream(databasePath);
        if (databaseStream.is_open()) {
            try {
                json jroot;
                databaseStream >> jroot;
                database = jroot;
                if (databaseStream.bad()) {
                    std::string u8string = databasePath.u8string();
                    fprintf(stderr, "Failed to read database file at %s.", u8string.c_str());
                    return 1;
                }
            }
            catch (const nlohmann::detail::exception &e) {
                fprintf(stderr, "JSON parsing error: %s\n", e.what());
                return 1;
            }
        }
    }
    else if (mode == Mode::Upgrade) {
        fprintf(stderr, "Database file rt64.json is missing.\n");
        return 1;
    }

    std::list<RenameFile> renameFileList;
    if (mode == Mode::Rice) {
        if (database.config.hashVersion < RT64::TMEMHasher::CurrentHashVersion) {
            fprintf(stderr, "Database hash version (%u) is older than texture hasher's version (%u). Upgrade it first using the --upgrade command.\n", database.config.hashVersion, RT64::TMEMHasher::CurrentHashVersion);
            return 1;
        }

        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(searchDirectory)) {
            if (entry.is_regular_file()) {
                const std::string filename = entry.path().filename().u8string();
                if (endsWith(filename, RiceInfoExtension)) {
                    addRiceHash(searchDirectory, filename.substr(0, filename.size() - RiceInfoExtension.size()), database, renameFileList);
                }
            }
        }
    }
    else if (mode == Mode::Upgrade) {
        if (database.config.hashVersion > RT64::TMEMHasher::CurrentHashVersion) {
            fprintf(stderr, "Database hash version (%u) is newer than texture hasher's version (%u).\n", database.config.hashVersion, RT64::TMEMHasher::CurrentHashVersion);
            return 1;
        }
        else if (database.config.hashVersion == RT64::TMEMHasher::CurrentHashVersion) {
            fprintf(stderr, "Database hash version is already up to date.\n");
            return 1;
        }

        for (RT64::ReplacementTexture &texture : database.textures) {
            upgradeHash(searchDirectory, texture.hashes.rt64, database, renameFileList);
        }

        database.config.hashVersion = RT64::TMEMHasher::CurrentHashVersion;
    }

    // Save new database file.
    std::filesystem::path databaseNewPath = databasePath.u8string() + ".new";
    std::ofstream databaseNewStream(databaseNewPath);
    if (!databaseNewStream.is_open()) {
        std::string u8string = databaseNewPath.u8string();
        fprintf(stderr, "Failed to open database file at %s for writing.", u8string.c_str());
        return 1;
    }

    try {
        json jroot = database;
        databaseNewStream << std::setw(4) << jroot << std::endl;
        if (!databaseNewStream.bad()) {
            databaseNewStream.close();
        }
        else {
            std::string u8string = databaseNewPath.u8string();
            fprintf(stderr, "Failed to write to database file at %s.", u8string.c_str());
            return 1;
        }
    }
    catch (const nlohmann::detail::exception &e) {
        fprintf(stderr, "JSON writing error: %s\n", e.what());
        return 1;
    }

    // If everything went OK, swap the names and delete the old file.
    std::error_code ec;
    std::filesystem::path databaseOldPath = databasePath.u8string() + ".old";
    if (std::filesystem::exists(databasePath)) {
        if (std::filesystem::exists(databaseOldPath)) {
            std::filesystem::remove(databaseOldPath, ec);
            if (ec) {
                std::string u8string = databaseOldPath.u8string();
                fprintf(stderr, "Failed to remove old database file at %s.", u8string.c_str());
                return 1;
            }
        }

        std::filesystem::rename(databasePath, databaseOldPath, ec);
        if (ec) {
            std::string u8stringOld = databaseOldPath.u8string();
            std::string u8stringNew = databaseNewPath.u8string();
            fprintf(stderr, "Failed to rename old database file from %s to %s.\n", u8stringOld.c_str(), u8stringNew.c_str());
            return 1;
        }
    }

    std::filesystem::rename(databaseNewPath, databasePath, ec);
    if (ec) {
        std::string u8stringNew = databaseNewPath.u8string();
        std::string u8stringCur = databasePath.u8string();
        fprintf(stderr, "Failed to rename new database file from %s to %s.\n", u8stringNew.c_str(), u8stringCur.c_str());
        return 1;
    }

    // Once the database has been upgraded successfully, rename all the files that were detected.
    if (!renameFileList.empty()) {
        executeRenameFiles(renameFileList);
    }
    
    return 0;
}
#endif
