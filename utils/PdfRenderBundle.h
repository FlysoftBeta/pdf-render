// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef PDF_RENDER_BUNDLE_H
#define PDF_RENDER_BUNDLE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class PdfRenderBundle
{
public:
    // Adds a path to the bundle and returns the content-addressed file id.
    // Identical content is stored only once, even when it has several paths.
    std::string addFile(std::string path, std::string mime, std::vector<unsigned char> data);
    std::string addText(std::string path, std::string mime, std::string data);

    bool write(const std::filesystem::path &path, std::string *error) const;

private:
    struct Blob
    {
        std::string id;
        std::string mime;
        std::vector<unsigned char> data;
    };

    struct File
    {
        std::string path;
        std::size_t blob;
    };

    std::vector<Blob> blobs;
    std::vector<File> files;
    std::unordered_map<std::string, std::size_t> blobsById;
    std::unordered_map<std::string, std::size_t> filesByPath;
};

#endif
