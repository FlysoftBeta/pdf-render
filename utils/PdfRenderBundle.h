// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef PDF_RENDER_BUNDLE_H
#define PDF_RENDER_BUNDLE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Produces a standard ZIP64 container whose object entries are already
// compressed according to the ClassApp render manifest. ZIP itself always
// stores entries verbatim so the server can extract individual payloads.
class PdfRenderBundle
{
public:
    PdfRenderBundle();
    ~PdfRenderBundle();
    PdfRenderBundle(const PdfRenderBundle &) = delete;
    PdfRenderBundle &operator=(const PdfRenderBundle &) = delete;

    std::string addFile(std::string path, std::string mime, std::vector<unsigned char> data);
    std::string addText(std::string path, std::string mime, std::string data);
    bool addPath(std::string path, const std::string &contentId);
    void setDocumentManifest(std::string jsonObject);

    bool write(const std::filesystem::path &path, std::string *error) const;

private:
    struct Blob
    {
        std::string id;
        std::string mime;
        std::filesystem::path temporaryPath;
        std::uint64_t size = 0;
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
    std::filesystem::path temporaryDirectory;
    std::string documentManifest = "{}";
};

#endif
