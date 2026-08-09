// SPDX-License-Identifier: GPL-2.0-or-later

#include "PdfRenderBundle.h"

#include <zdict.h>
#include <zstd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <type_traits>

namespace {

constexpr std::size_t maxDictionarySize = 64 * 1024;
constexpr std::size_t maxTrainingBytes = 8 * 1024 * 1024;
constexpr std::size_t maxSampleBytes = 128 * 1024;
constexpr int compressionLevel = 8;

constexpr std::array<std::uint32_t, 64> sha256Constants {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138,
    0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c,
    0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

std::uint32_t rotateRight(std::uint32_t value, int count)
{
    return (value >> count) | (value << (32 - count));
}

void sha256Block(std::array<std::uint32_t, 8> *hash, const unsigned char *block)
{
    std::array<std::uint32_t, 64> words {};
    for (std::size_t i = 0; i < 16; ++i) {
        const std::size_t pos = i * 4;
        words[i] = (static_cast<std::uint32_t>(block[pos]) << 24) | (static_cast<std::uint32_t>(block[pos + 1]) << 16) | (static_cast<std::uint32_t>(block[pos + 2]) << 8) | block[pos + 3];
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const std::uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const std::uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = *hash;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + sum1 + choice + sha256Constants[i] + words[i];
        const std::uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    const std::array<std::uint32_t, 8> result { a, b, c, d, e, f, g, h };
    for (std::size_t i = 0; i < hash->size(); ++i) {
        (*hash)[i] += result[i];
    }
}

std::array<unsigned char, 32> sha256(const std::vector<unsigned char> &input)
{
    std::array<std::uint32_t, 8> hash { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::size_t offset = 0;
    while (offset + 64 <= input.size()) {
        sha256Block(&hash, input.data() + offset);
        offset += 64;
    }
    std::array<unsigned char, 128> tail {};
    const std::size_t remaining = input.size() - offset;
    if (remaining) std::copy_n(input.data() + offset, remaining, tail.data());
    tail[remaining] = 0x80;
    const std::size_t paddedSize = remaining < 56 ? 64 : 128;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[paddedSize - 1 - index] = static_cast<unsigned char>(bitLength >> (index * 8));
    }
    sha256Block(&hash, tail.data());
    if (paddedSize == 128) sha256Block(&hash, tail.data() + 64);

    std::array<unsigned char, 32> result {};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        result[i * 4] = static_cast<unsigned char>(hash[i] >> 24);
        result[i * 4 + 1] = static_cast<unsigned char>(hash[i] >> 16);
        result[i * 4 + 2] = static_cast<unsigned char>(hash[i] >> 8);
        result[i * 4 + 3] = static_cast<unsigned char>(hash[i]);
    }
    return result;
}

std::string toHex(const std::array<unsigned char, 32> &value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        result[i * 2] = digits[value[i] >> 4];
        result[i * 2 + 1] = digits[value[i] & 15];
    }
    return result;
}

template<typename T>
void writeLittleEndian(std::ostream &out, T value)
{
    static_assert(std::is_unsigned_v<T>);
    std::array<unsigned char, sizeof(T)> bytes {};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(value >> (i * 8));
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::uint64_t position(std::ostream &out)
{
    return static_cast<std::uint64_t>(out.tellp());
}

bool isTextMime(std::string_view mime)
{
    return mime.starts_with("text/");
}

std::string jsonEscape(std::string_view value)
{
    std::ostringstream escaped;
    for (const unsigned char c : value) {
        switch (c) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                escaped << "\\u00" << digits[c >> 4] << digits[c & 15];
            } else {
                escaped << static_cast<char>(c);
            }
        }
    }
    return escaped.str();
}

std::vector<unsigned char> readFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto length = input.tellg();
    if (length < 0) return {};
    std::vector<unsigned char> data(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(data.data()), data.size());
    return input ? data : std::vector<unsigned char> {};
}

std::uint32_t checksum(const std::vector<unsigned char> &data)
{
    uLong value = crc32(0L, Z_NULL, 0);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t amount = std::min<std::size_t>(data.size() - offset, std::numeric_limits<uInt>::max());
        value = crc32(value, data.data() + offset, static_cast<uInt>(amount));
        offset += amount;
    }
    return static_cast<std::uint32_t>(value);
}

struct ZipEntry
{
    std::string name;
    std::uint32_t crc = 0;
    std::uint64_t size = 0;
    std::uint64_t localOffset = 0;
    std::uint64_t dataOffset = 0;
};

bool beginZipEntry(std::ostream &out, std::string name, std::uint32_t crc, std::uint64_t size, ZipEntry *entry)
{
    if (name.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    *entry = { std::move(name), crc, size, position(out) };
    writeLittleEndian<std::uint32_t>(out, 0x04034b50);
    writeLittleEndian<std::uint16_t>(out, 45);
    writeLittleEndian<std::uint16_t>(out, 0x0800); // UTF-8 names.
    writeLittleEndian<std::uint16_t>(out, 0); // STORED: payload is precompressed.
    writeLittleEndian<std::uint16_t>(out, 0);
    writeLittleEndian<std::uint16_t>(out, 0);
    writeLittleEndian<std::uint32_t>(out, entry->crc);
    writeLittleEndian<std::uint32_t>(out, 0xffffffff);
    writeLittleEndian<std::uint32_t>(out, 0xffffffff);
    writeLittleEndian<std::uint16_t>(out, static_cast<std::uint16_t>(entry->name.size()));
    writeLittleEndian<std::uint16_t>(out, 20);
    out.write(entry->name.data(), entry->name.size());
    writeLittleEndian<std::uint16_t>(out, 0x0001);
    writeLittleEndian<std::uint16_t>(out, 16);
    writeLittleEndian<std::uint64_t>(out, entry->size);
    writeLittleEndian<std::uint64_t>(out, entry->size);
    entry->dataOffset = position(out);
    return !!out;
}

bool writeZipEntry(std::ostream &out, std::string name, const std::vector<unsigned char> &data, std::vector<ZipEntry> *entries)
{
    ZipEntry entry;
    if (!beginZipEntry(out, std::move(name), checksum(data), data.size(), &entry)) return false;
    out.write(reinterpret_cast<const char *>(data.data()), data.size());
    entries->push_back(std::move(entry));
    return !!out;
}

bool checksumFile(const std::filesystem::path &path, std::uint64_t expectedSize, std::uint32_t *result)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::array<unsigned char, 32 * 1024> buffer {};
    std::uint64_t total = 0;
    uLong value = crc32(0L, Z_NULL, 0);
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const auto amount = input.gcount();
        if (amount <= 0) break;
        value = crc32(value, buffer.data(), static_cast<uInt>(amount));
        total += static_cast<std::uint64_t>(amount);
    }
    if (!input.eof() || total != expectedSize) return false;
    *result = static_cast<std::uint32_t>(value);
    return true;
}

bool writeZipFileEntry(std::ostream &out, std::string name, const std::filesystem::path &path, std::uint64_t size, std::vector<ZipEntry> *entries)
{
    std::uint32_t crc = 0;
    if (!checksumFile(path, size, &crc)) return false;
    ZipEntry entry;
    if (!beginZipEntry(out, std::move(name), crc, size, &entry)) return false;
    std::ifstream input(path, std::ios::binary);
    std::array<char, 32 * 1024> buffer {};
    std::uint64_t total = 0;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto amount = input.gcount();
        if (amount <= 0) break;
        out.write(buffer.data(), amount);
        total += static_cast<std::uint64_t>(amount);
    }
    if (!input.eof() || total != size || !out) return false;
    entries->push_back(std::move(entry));
    return true;
}

bool finishZip(std::ostream &out, const std::vector<ZipEntry> &entries)
{
    const std::uint64_t centralOffset = position(out);
    for (const auto &entry : entries) {
        writeLittleEndian<std::uint32_t>(out, 0x02014b50);
        writeLittleEndian<std::uint16_t>(out, 45);
        writeLittleEndian<std::uint16_t>(out, 45);
        writeLittleEndian<std::uint16_t>(out, 0x0800);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint32_t>(out, entry.crc);
        writeLittleEndian<std::uint32_t>(out, 0xffffffff);
        writeLittleEndian<std::uint32_t>(out, 0xffffffff);
        writeLittleEndian<std::uint16_t>(out, static_cast<std::uint16_t>(entry.name.size()));
        writeLittleEndian<std::uint16_t>(out, 28);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint16_t>(out, 0);
        writeLittleEndian<std::uint32_t>(out, 0);
        writeLittleEndian<std::uint32_t>(out, 0xffffffff);
        out.write(entry.name.data(), entry.name.size());
        writeLittleEndian<std::uint16_t>(out, 0x0001);
        writeLittleEndian<std::uint16_t>(out, 24);
        writeLittleEndian<std::uint64_t>(out, entry.size);
        writeLittleEndian<std::uint64_t>(out, entry.size);
        writeLittleEndian<std::uint64_t>(out, entry.localOffset);
    }
    const std::uint64_t centralSize = position(out) - centralOffset;
    const std::uint64_t zip64Offset = position(out);
    writeLittleEndian<std::uint32_t>(out, 0x06064b50);
    writeLittleEndian<std::uint64_t>(out, 44);
    writeLittleEndian<std::uint16_t>(out, 45);
    writeLittleEndian<std::uint16_t>(out, 45);
    writeLittleEndian<std::uint32_t>(out, 0);
    writeLittleEndian<std::uint32_t>(out, 0);
    writeLittleEndian<std::uint64_t>(out, entries.size());
    writeLittleEndian<std::uint64_t>(out, entries.size());
    writeLittleEndian<std::uint64_t>(out, centralSize);
    writeLittleEndian<std::uint64_t>(out, centralOffset);
    writeLittleEndian<std::uint32_t>(out, 0x07064b50);
    writeLittleEndian<std::uint32_t>(out, 0);
    writeLittleEndian<std::uint64_t>(out, zip64Offset);
    writeLittleEndian<std::uint32_t>(out, 1);
    writeLittleEndian<std::uint32_t>(out, 0x06054b50);
    writeLittleEndian<std::uint16_t>(out, 0);
    writeLittleEndian<std::uint16_t>(out, 0);
    writeLittleEndian<std::uint16_t>(out, 0xffff);
    writeLittleEndian<std::uint16_t>(out, 0xffff);
    writeLittleEndian<std::uint32_t>(out, 0xffffffff);
    writeLittleEndian<std::uint32_t>(out, 0xffffffff);
    writeLittleEndian<std::uint16_t>(out, 0);
    return !!out;
}

} // namespace

PdfRenderBundle::PdfRenderBundle()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto random = std::random_device {}();
    temporaryDirectory = std::filesystem::temp_directory_path() / ("pdfrender-" + std::to_string(stamp) + "-" + std::to_string(random));
    std::error_code error;
    std::filesystem::create_directories(temporaryDirectory, error);
}

PdfRenderBundle::~PdfRenderBundle()
{
    std::error_code ignored;
    std::filesystem::remove_all(temporaryDirectory, ignored);
}

std::string PdfRenderBundle::addFile(std::string path, std::string mime, std::vector<unsigned char> data)
{
    const std::string id = toHex(sha256(data));
    std::size_t blobIndex;
    const auto existingBlob = blobsById.find(id);
    if (existingBlob == blobsById.end()) {
        blobIndex = blobs.size();
        blobsById.emplace(id, blobIndex);
        const std::filesystem::path temporaryPath = temporaryDirectory / id;
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(data.data()), data.size());
        if (!output) {
            return {};
        }
        blobs.push_back({ id, std::move(mime), temporaryPath, data.size() });
    } else {
        blobIndex = existingBlob->second;
        if (blobs[blobIndex].mime != mime) {
            return {};
        }
    }
    if (!path.empty()) {
        const auto existingFile = filesByPath.find(path);
        if (existingFile == filesByPath.end()) {
            filesByPath.emplace(path, files.size());
            files.push_back({ std::move(path), blobIndex });
        } else {
            files[existingFile->second].blob = blobIndex;
        }
    }
    return id;
}

bool PdfRenderBundle::addPath(std::string path, const std::string &contentId)
{
    const auto blob = blobsById.find(contentId);
    if (blob == blobsById.end()) {
        return false;
    }
    const auto existingFile = filesByPath.find(path);
    if (existingFile == filesByPath.end()) {
        filesByPath.emplace(path, files.size());
        files.push_back({ std::move(path), blob->second });
    } else {
        files[existingFile->second].blob = blob->second;
    }
    return true;
}

std::string PdfRenderBundle::addText(std::string path, std::string mime, std::string data)
{
    return addFile(std::move(path), std::move(mime), { data.begin(), data.end() });
}

void PdfRenderBundle::setDocumentManifest(std::string jsonObject)
{
    documentManifest = std::move(jsonObject);
}

bool PdfRenderBundle::write(const std::filesystem::path &path, std::string *error) const
{
    std::vector<unsigned char> samples;
    std::vector<std::size_t> sampleSizes;
    for (const auto &blob : blobs) {
        if (!isTextMime(blob.mime) || blob.size == 0 || samples.size() >= maxTrainingBytes) continue;
        std::ifstream input(blob.temporaryPath, std::ios::binary);
        const std::size_t amount = std::min<std::size_t>({ static_cast<std::size_t>(blob.size), maxSampleBytes, maxTrainingBytes - samples.size() });
        const std::size_t start = samples.size();
        samples.resize(start + amount);
        input.read(reinterpret_cast<char *>(samples.data() + start), amount);
        if (!input) {
            *error = "cannot read spooled render resource";
            return false;
        }
        sampleSizes.push_back(amount);
    }
    std::vector<unsigned char> dictionary;
    if (sampleSizes.size() >= 4 && samples.size() >= 4096) {
        const std::size_t capacity = std::min(maxDictionarySize, std::max<std::size_t>(1024, samples.size() / 8));
        dictionary.resize(capacity);
        const std::size_t result = ZDICT_trainFromBuffer(dictionary.data(), dictionary.size(), samples.data(), sampleSizes.data(), static_cast<unsigned int>(sampleSizes.size()));
        if (ZDICT_isError(result) || result == 0) dictionary.clear();
        else dictionary.resize(result);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "cannot open output file: " + path.string();
        return false;
    }
    std::vector<ZipEntry> entries;
    std::ostringstream resources;
    resources << '[';
    bool firstResource = true;

    if (!dictionary.empty() && !writeZipEntry(out, "dictionary.zdict", dictionary, &entries)) {
        *error = "cannot write dictionary entry";
        return false;
    }

    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), ZSTD_freeCCtx);
    std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> compiled(
        dictionary.empty() ? nullptr : ZSTD_createCDict(dictionary.data(), dictionary.size(), compressionLevel),
        ZSTD_freeCDict);
    if (!context || (!dictionary.empty() && !compiled)) {
        *error = "cannot create zstd compression context";
        return false;
    }

    for (const auto &blob : blobs) {
        const bool compress = isTextMime(blob.mime);
        std::string encoding = "identity";
        std::uint64_t storedSize = blob.size;
        const std::string entryPath = "objects/" + blob.id + (compress ? ".zst" : "");
        if (compress) {
            const std::vector<unsigned char> raw = readFile(blob.temporaryPath);
            if (raw.size() != blob.size) {
                *error = "cannot read spooled render resource";
                return false;
            }
            std::vector<unsigned char> stored;
            stored.resize(ZSTD_compressBound(raw.size()));
            const std::size_t size = compiled
                ? ZSTD_compress_usingCDict(context.get(), stored.data(), stored.size(), raw.data(), raw.size(), compiled.get())
                : ZSTD_compressCCtx(context.get(), stored.data(), stored.size(), raw.data(), raw.size(), compressionLevel);
            if (ZSTD_isError(size)) {
                *error = std::string("zstd compression failed: ") + ZSTD_getErrorName(size);
                return false;
            }
            stored.resize(size);
            storedSize = stored.size();
            encoding = compiled ? "zstd-dictionary" : "zstd";
            if (!writeZipEntry(out, entryPath, stored, &entries)) {
                *error = "cannot write resource entry " + entryPath;
                return false;
            }
        } else if (!writeZipFileEntry(out, entryPath, blob.temporaryPath, blob.size, &entries)) {
            *error = "cannot write resource entry " + entryPath;
            return false;
        }
        if (!firstResource) resources << ',';
        firstResource = false;
        resources << "{\"contentId\":\"" << blob.id << "\",\"mime\":\"" << jsonEscape(blob.mime)
                  << "\",\"encoding\":\"" << encoding << "\",\"rawSize\":" << blob.size
                  << ",\"storedSize\":" << storedSize << ",\"storedOffset\":" << entries.back().dataOffset
                  << ",\"path\":\"" << entryPath << "\"}";
    }
    resources << ']';

    std::ostringstream paths;
    paths << '[';
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i) paths << ',';
        paths << "{\"path\":\"" << jsonEscape(files[i].path) << "\",\"contentId\":\"" << blobs[files[i].blob].id << "\"}";
    }
    paths << ']';

    std::ostringstream manifest;
    manifest << "{\"format\":\"classapp-render-archive\",\"version\":1,\"dictionary\":";
    if (dictionary.empty()) {
        manifest << "null";
    } else {
        manifest << "{\"contentId\":\"" << toHex(sha256(dictionary)) << "\",\"path\":\"dictionary.zdict\",\"size\":" << dictionary.size()
                 << ",\"storedOffset\":" << entries.front().dataOffset << '}';
    }
    manifest << ",\"resources\":" << resources.str() << ",\"files\":" << paths.str() << ",\"document\":" << documentManifest << '}';
    const std::string manifestText = manifest.str();
    const std::vector<unsigned char> manifestBytes(manifestText.begin(), manifestText.end());
    if (!writeZipEntry(out, "manifest.json", manifestBytes, &entries) || !finishZip(out, entries)) {
        *error = "cannot finalize render archive";
        return false;
    }
    return true;
}
