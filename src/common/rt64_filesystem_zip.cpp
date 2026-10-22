//
// RT64
//

#include "rt64_filesystem_zip.h"

#include <miniz/miniz.h>
#include <zstd.h>

#include "rt64_mapped_file.h"

namespace RT64 {
    enum {
        MZ_ZIP_LOCAL_DIR_HEADER_SIG = 0x04034b50,
        MZ_ZIP_LOCAL_DIR_HEADER_SIZE = 30,
        MZ_ZIP_LDH_FILENAME_LEN_OFS = 26,
        MZ_ZIP_LDH_EXTRA_LEN_OFS = 28,
        MZ_ZSTD = 93,
    };

    static bool startsWith(const std::string &str, const std::string &start) {
        if (str.length() >= start.length()) {
            return (str.compare(0, start.length(), start) == 0);
        }
        else {
            return false;
        }
    }

    // The ZIP format uses CP437 when the UTF-8 flag is unset. Decode names
    // once so exact entry paths can be persisted losslessly in UTF-8 JSON.
    static std::string decodeZipName(const std::string &name, bool utf8) {
        if (utf8)
            return name;
        static const uint16_t cp437[128] = {
            0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
            0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
            0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
            0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
            0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
            0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
            0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
            0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
            0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
            0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
            0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
            0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
            0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
            0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
            0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
            0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0
        };
        std::string result;
        for (unsigned char c : name) {
            if (c < 128) {
                result += char(c);
                continue;
            }
            const auto code = cp437[c - 128];
            if (code < 0x800) {
                result += char(0xc0 | (code >> 6));
                result += char(0x80 | (code & 0x3f));
            }
            else {
                result += char(0xe0 | (code >> 12));
                result += char(0x80 | ((code >> 6) & 0x3f));
                result += char(0x80 | (code & 0x3f));
            }
        }
        return result;
    }

    // FileSystemZip::Implementation

    struct FileSystemZip::Implementation {
        std::unordered_map<std::string, FileSystemZipInfo> fileInfoMap;
        std::shared_ptr<FileSystemZipIterator> endIterator;
        std::filesystem::path zipPath;
        MappedFile zipMappedFile;
        std::string basePath;
        bool archiveOpen = false;
    };

    // FileSystemZip

    FileSystemZip::FileSystemZip(const std::filesystem::path &zipPath, const std::string &basePath) {
        assert(basePath.empty() || (basePath.back() != '\\') || (basePath.back() != '/'));

        impl = new Implementation();
        if (!impl->zipMappedFile.open(zipPath)) {
            return;
        }

        mz_zip_archive zipArchive = {};
        std::string zipPathStr = zipPath.u8string();
        impl->archiveOpen = mz_zip_reader_init_mem(&zipArchive, impl->zipMappedFile.data(), impl->zipMappedFile.size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY);
        impl->basePath = basePath.empty() ? std::string() : (basePath + "/");
        impl->zipPath = zipPath;
        
        if (impl->archiveOpen) {
            uint32_t fileCount = mz_zip_reader_get_num_files(&zipArchive);
            for (uint32_t i = 0; i < fileCount; i++) {
                mz_zip_archive_file_stat fileStat = {};
                if (!mz_zip_reader_file_stat(&zipArchive, i, &fileStat)) {
                    impl->archiveOpen = false;
                    break;
                }

                // Ignore directories.
                if (fileStat.m_is_directory) {
                    continue;
                }

                // Ignore entries that don't start with the base path. Take out the base path out of the filename.
                const mz_uint nameLength = mz_zip_reader_get_filename(&zipArchive, i, nullptr, 0);
                std::string fileName(nameLength, '\0');
                if (!nameLength || !mz_zip_reader_get_filename(&zipArchive, i, fileName.data(), nameLength)) {
                    impl->archiveOpen = false;
                    break;
                }
                fileName.resize(nameLength - 1);
                fileName = FileSystem::toForwardSlashes(decodeZipName(fileName, (fileStat.m_bit_flag & (1 << 11)) != 0));
                if (!impl->basePath.empty()) {
                    if (!startsWith(fileName, impl->basePath)) {
                        continue;
                    }

                    fileName = fileName.substr(impl->basePath.size());
                }

                // Detect compression method.
                FileSystemZipInfo::Compression compression;
                switch (fileStat.m_method) {
                case 0:
                    compression = FileSystemZipInfo::Compression::None;
                    break;
                case MZ_DEFLATED:
                    compression = FileSystemZipInfo::Compression::Deflate;
                    break;
                case MZ_ZSTD:
                    compression = FileSystemZipInfo::Compression::Zstd;
                    break;
                default:
                    // Unknown compression method, ignore the file.
                    continue;
                }

                // Store the information necessary for extracting the file later.
                if (impl->fileInfoMap.find(fileName) != impl->fileInfoMap.end() || fileStat.m_is_encrypted) {
                    impl->archiveOpen = false;
                    break;
                }
                FileSystemZipInfo &fileInfo = impl->fileInfoMap[fileName];
                fileInfo.localHeaderOffset = fileStat.m_local_header_ofs;
                fileInfo.compressedSize = fileStat.m_comp_size;
                fileInfo.uncompressedSize = fileStat.m_uncomp_size;
                fileInfo.checksumCRC32 = fileStat.m_crc32;
                fileInfo.compression = compression;
            }

            mz_zip_reader_end(&zipArchive);
        }

        impl->endIterator = { std::make_shared<FileSystemZipIterator>(impl->fileInfoMap.end()) };
    }

    FileSystemZip::~FileSystemZip() {
        delete impl;
    }

    FileSystem::Iterator FileSystemZip::begin() const {
        return { std::make_shared<FileSystemZipIterator>(impl->fileInfoMap.begin()) };
    }
    
    FileSystem::Iterator FileSystemZip::end() const {
        return { impl->endIterator };
    }
    
    bool FileSystemZip::load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const {
        assert(impl->archiveOpen);

        auto it = impl->fileInfoMap.find(path);
        if (it == impl->fileInfoMap.end()) {
            return false;
        }

        // Must have enough bytes to read the entire file.
        if (fileDataMaxByteCount < it->second.uncompressedSize) {
            return false;
        }

        const auto &info = it->second;
        if (info.localHeaderOffset > impl->zipMappedFile.size() ||
            MZ_ZIP_LOCAL_DIR_HEADER_SIZE > impl->zipMappedFile.size() - info.localHeaderOffset)
            return false;
        // Validate the local dir header.
        const char *localDirHeader = reinterpret_cast<const char *>(&impl->zipMappedFile.data()[it->second.localHeaderOffset]);
        if (MZ_READ_LE32(localDirHeader) != MZ_ZIP_LOCAL_DIR_HEADER_SIG) {
            return false;
        }

        // Skip over unused data of the header.
        uint32_t ldhFilenameLenOfs = MZ_READ_LE16(localDirHeader + MZ_ZIP_LDH_FILENAME_LEN_OFS);
        uint32_t ldhExtraLenOfs = MZ_READ_LE16(localDirHeader + MZ_ZIP_LDH_EXTRA_LEN_OFS);
        size_t dataAddress = it->second.localHeaderOffset + MZ_ZIP_LOCAL_DIR_HEADER_SIZE + ldhFilenameLenOfs + ldhExtraLenOfs;

        if (dataAddress > impl->zipMappedFile.size() ||
            info.compressedSize > impl->zipMappedFile.size() - dataAddress)
            return false;
        // Make sure output vector has enough bytes to hold the data.
        const uint8_t *zipFileData = reinterpret_cast<const uint8_t *>(&impl->zipMappedFile.data()[dataAddress]);
        if (it->second.compression == FileSystemZipInfo::Compression::Zstd) {
            if (ZSTD_decompress(fileData, it->second.uncompressedSize, zipFileData, it->second.compressedSize) != it->second.uncompressedSize) {
                return false;
            }
        }
        else if (it->second.compression == FileSystemZipInfo::Compression::Deflate) {
            if (tinfl_decompress_mem_to_mem(fileData, it->second.uncompressedSize, zipFileData, it->second.compressedSize, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) != it->second.uncompressedSize) {
                return false;
            }
        }
        else {
            if (info.compressedSize != info.uncompressedSize)
                return false;
            memcpy(fileData, zipFileData, it->second.uncompressedSize);
        }

        return mz_crc32(MZ_CRC32_INIT, fileData, info.uncompressedSize) == info.checksumCRC32;
    }

    size_t FileSystemZip::getSize(const std::string &path) const {
        assert(impl->archiveOpen);
        auto it = impl->fileInfoMap.find(path);
        if (it != impl->fileInfoMap.end()) {
            return it->second.uncompressedSize;
        }
        else {
            return 0;
        }
    }
    
    bool FileSystemZip::exists(const std::string &path) const {
        assert(impl->archiveOpen);

        auto it = impl->fileInfoMap.find(path);
        return (it != impl->fileInfoMap.end());
    }

    std::string FileSystemZip::makeCanonical(const std::string &path) const {
        // Has no effect on Zip files which are already assumed to be validated for case sensitivity.
        return path;
    }

    bool FileSystemZip::isOpen() const {
        return impl->archiveOpen;
    }

    std::unique_ptr<FileSystem> FileSystemZip::create(const std::filesystem::path &zipPath, const std::string &basePath) {
        std::unique_ptr<FileSystemZip> zipFileSystem = std::make_unique<FileSystemZip>(zipPath, basePath);
        if (!zipFileSystem->isOpen()) {
            zipFileSystem.reset();
        }

        return zipFileSystem;
    }

    // FileSystemZipIterator

    FileSystemZipIterator::FileSystemZipIterator(std::unordered_map<std::string, FileSystemZipInfo>::iterator iterator) {
        this->iterator = iterator;
    }

    const std::string &FileSystemZipIterator::value() {
        return iterator->first;
    }

    void FileSystemZipIterator::increment() {
        iterator++;
    }

    bool FileSystemZipIterator::compare(const FileSystemIteratorImplementation *other) const {
        const FileSystemZipIterator *otherZipIt = static_cast<const FileSystemZipIterator *>(other);
        return (iterator == otherZipIt->iterator);
    }
};