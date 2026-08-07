// SPDX-License-Identifier: GPL-2.0-or-later

#include "PdfRenderBundle.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <type_traits>

namespace {

constexpr std::array<unsigned char, 8> bundleMagic { 'P', 'D', 'R', 'B', 'N', 'D', 'L', 0 };
constexpr std::size_t maxDictionarySize = 64 * 1024;
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

std::array<unsigned char, 32> sha256(const std::vector<unsigned char> &input)
{
    std::array<std::uint32_t, 8> hash { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::vector<unsigned char> message(input);
    std::uint64_t bitLength = message.size();
    bitLength *= 8;
    message.push_back(0x80);
    while ((message.size() % 64) != 56) {
        message.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<unsigned char>(bitLength >> shift));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<std::uint32_t, 64> words {};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t pos = offset + i * 4;
            words[i] = (static_cast<std::uint32_t>(message[pos]) << 24) | (static_cast<std::uint32_t>(message[pos + 1]) << 16) | (static_cast<std::uint32_t>(message[pos + 2]) << 8) | message[pos + 3];
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = hash;
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
        for (std::size_t i = 0; i < hash.size(); ++i) {
            hash[i] += result[i];
        }
    }

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

std::array<unsigned char, 32> fromHex(std::string_view value)
{
    std::array<unsigned char, 32> result {};
    auto nibble = [](char c) -> unsigned char { return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<unsigned char>((nibble(value[i * 2]) << 4) | nibble(value[i * 2 + 1]));
    }
    return result;
}

bool isTextMime(std::string_view mime)
{
    return mime.starts_with("text/");
}

template<typename Blobs>
std::vector<unsigned char> makeDictionary(const Blobs &blobs)
{
    std::vector<unsigned char> dictionary;
    std::size_t textBlobCount = 0;
    for (const auto &blob : blobs) {
        if (isTextMime(blob.mime) && !blob.data.empty()) {
            ++textBlobCount;
        }
    }
    if (textBlobCount == 0) {
        return dictionary;
    }

    // A raw-content zstd dictionary is valid without a trained dictionary
    // header. Take an even slice from every text asset so the single package
    // dictionary represents HTML, CSS, and the text-typed manifest.
    const std::size_t perBlob = std::max<std::size_t>(1, maxDictionarySize / textBlobCount);
    for (const auto &blob : blobs) {
        if (!isTextMime(blob.mime) || blob.data.empty() || dictionary.size() == maxDictionarySize) {
            continue;
        }
        const std::size_t amount = std::min({ perBlob, blob.data.size(), maxDictionarySize - dictionary.size() });
        dictionary.insert(dictionary.end(), blob.data.begin(), blob.data.begin() + amount);
    }
    return dictionary;
}

} // namespace

std::string PdfRenderBundle::addFile(std::string path, std::string mime, std::vector<unsigned char> data)
{
    const std::string id = toHex(sha256(data));
    std::size_t blobIndex;
    const auto existingBlob = blobsById.find(id);
    if (existingBlob == blobsById.end()) {
        blobIndex = blobs.size();
        blobsById.emplace(id, blobIndex);
        blobs.push_back({ id, std::move(mime), std::move(data) });
    } else {
        blobIndex = existingBlob->second;
    }

    if (path.empty()) {
        return id;
    }
    const auto existingFile = filesByPath.find(path);
    if (existingFile == filesByPath.end()) {
        filesByPath.emplace(path, files.size());
        files.push_back({ std::move(path), blobIndex });
    } else {
        files[existingFile->second].blob = blobIndex;
    }
    return id;
}

std::string PdfRenderBundle::addText(std::string path, std::string mime, std::string data)
{
    return addFile(std::move(path), std::move(mime), { data.begin(), data.end() });
}

bool PdfRenderBundle::write(const std::filesystem::path &path, std::string *error) const
{
    if (blobs.size() > std::numeric_limits<std::uint32_t>::max() || files.size() > std::numeric_limits<std::uint32_t>::max()) {
        *error = "bundle has too many records";
        return false;
    }
    const std::vector<unsigned char> dictionary = makeDictionary(blobs);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "cannot open output file: " + path.string();
        return false;
    }

    out.write(reinterpret_cast<const char *>(bundleMagic.data()), bundleMagic.size());
    writeLittleEndian<std::uint16_t>(out, 1); // version
    writeLittleEndian<std::uint16_t>(out, 1); // flags: little endian
    writeLittleEndian<std::uint32_t>(out, static_cast<std::uint32_t>(dictionary.size()));
    writeLittleEndian<std::uint32_t>(out, static_cast<std::uint32_t>(blobs.size()));
    writeLittleEndian<std::uint32_t>(out, static_cast<std::uint32_t>(files.size()));
    writeLittleEndian<std::uint32_t>(out, 0);
    out.write(reinterpret_cast<const char *>(dictionary.data()), dictionary.size());

    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (!context) {
        *error = "cannot create zstd compression context";
        return false;
    }
    for (const auto &blob : blobs) {
        if (blob.mime.size() > std::numeric_limits<std::uint16_t>::max()) {
            *error = "bundle MIME type is too long";
            ZSTD_freeCCtx(context);
            return false;
        }
        std::vector<unsigned char> stored;
        const bool compress = isTextMime(blob.mime);
        if (compress) {
            stored.resize(ZSTD_compressBound(blob.data.size()));
            const std::size_t size = ZSTD_compress_usingDict(context, stored.data(), stored.size(), blob.data.data(), blob.data.size(), dictionary.data(), dictionary.size(), compressionLevel);
            if (ZSTD_isError(size)) {
                *error = std::string("zstd compression failed: ") + ZSTD_getErrorName(size);
                ZSTD_freeCCtx(context);
                return false;
            }
            stored.resize(size);
        } else {
            stored = blob.data;
        }

        const auto hash = fromHex(blob.id);
        out.write(reinterpret_cast<const char *>(hash.data()), hash.size());
        writeLittleEndian<std::uint16_t>(out, static_cast<std::uint16_t>(blob.mime.size()));
        out.put(compress ? 1 : 0);
        out.put(0);
        writeLittleEndian<std::uint64_t>(out, blob.data.size());
        writeLittleEndian<std::uint64_t>(out, stored.size());
        out.write(blob.mime.data(), blob.mime.size());
        out.write(reinterpret_cast<const char *>(stored.data()), stored.size());
    }
    ZSTD_freeCCtx(context);

    for (const auto &file : files) {
        if (file.path.size() > std::numeric_limits<std::uint16_t>::max()) {
            *error = "bundle path is too long: " + file.path;
            return false;
        }
        writeLittleEndian<std::uint16_t>(out, static_cast<std::uint16_t>(file.path.size()));
        writeLittleEndian<std::uint16_t>(out, 0);
        const auto hash = fromHex(blobs[file.blob].id);
        out.write(reinterpret_cast<const char *>(hash.data()), hash.size());
        out.write(file.path.data(), file.path.size());
    }
    if (!out) {
        *error = "failed while writing output file: " + path.string();
        return false;
    }
    return true;
}
