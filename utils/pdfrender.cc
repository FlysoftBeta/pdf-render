// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include "PdfRenderBundle.h"
#include "goo/GooString.h"
#include "GlobalParams.h"
#include "PDFDoc.h"
#include "PDFDocFactory.h"
#include "TextOutputDev.h"
#include "CairoOutputDev.h"

#include <cairo-svg.h>
#include <cairo.h>

#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <webp/encode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csetjmp>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// Win32Console.h remaps stdio functions with macros. Include it only after
// headers that may declare methods with those names (notably Stream::printf).
#include "Win32Console.h"

namespace {

struct Options
{
    std::string input;
    std::string output;
    int firstPage = 1;
    int lastPage = 0;
    double resolution = 96.0;
    double webpQuality = 80.0;
    int jobs = 0;
    std::optional<GooString> ownerPassword;
    std::optional<GooString> userPassword;
};

class BackgroundOutputDev final : public CairoOutputDev
{
public:
    // Type 3 glyphs are small PDF programs, not ordinary font outlines. Ask
    // Gfx to interpret those programs so their paths/images remain in the SVG.
    bool interpretType3Chars() override { return true; }

    // Ordinary text is represented by positioned HTML instead.
    void beginString(GfxState *, const std::string &) override { }
    void drawChar(GfxState *, double, double, double, double, double, double, CharCode, int, const Unicode *, int) override { }
    void endString(GfxState *) override { }
};

class HtmlTextOutputDev final : public TextOutputDev
{
public:
    HtmlTextOutputDev() : TextOutputDev(nullptr, false, 0, false, false, false) { }

    // Type 3 glyph programs are already preserved in the background SVG.
    // Suppress their Unicode approximation here to avoid painting them twice.
    bool interpretType3Chars() override { return true; }
    bool beginType3Char(GfxState *, double, double, double, double, CharCode, const Unicode *, int) override { return true; }
};

struct MemoryStream
{
    std::vector<unsigned char> data;
};

cairo_status_t writeSvg(void *closure, const unsigned char *data, unsigned int length)
{
    auto *stream = static_cast<MemoryStream *>(closure);
    stream->data.insert(stream->data.end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

void usage(const char *program)
{
    std::cerr << "Usage: " << program << " [options] input.pdf output.pdrb\n"
              << "Options:\n"
              << "  -f, --first-page N     first page (default: 1)\n"
              << "  -l, --last-page N      last page (default: end)\n"
              << "  -r, --resolution DPI   CSS/SVG resolution (default: 96)\n"
              << "  -q, --webp-quality Q   WebP quality, 0-100 (default: 80)\n"
              << "  -j, --jobs N           worker threads, 1-256 (default: CPU count)\n"
              << "  --owner-password PASS  PDF owner password\n"
              << "  --user-password PASS   PDF user password\n"
              << "  -h, --help             show this help\n";
}

bool parseInteger(const char *text, int *value)
{
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!text[0] || *end || parsed < 1 || parsed > 1000000) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool parseDouble(const char *text, double minimum, double maximum, double *value)
{
    char *end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (!text[0] || *end || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parseOptions(int argc, char **argv, Options *options)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        auto requireValue = [&](const char *name) -> const char * {
            if (++i == argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[i];
        };
        if (arg == "-f" || arg == "--first-page") {
            const char *value = requireValue(argv[i]);
            if (!value || !parseInteger(value, &options->firstPage)) {
                std::cerr << "invalid first page\n";
                return false;
            }
        } else if (arg == "-l" || arg == "--last-page") {
            const char *value = requireValue(argv[i]);
            if (!value || !parseInteger(value, &options->lastPage)) {
                std::cerr << "invalid last page\n";
                return false;
            }
        } else if (arg == "-r" || arg == "--resolution") {
            const char *value = requireValue(argv[i]);
            if (!value || !parseDouble(value, 36, 600, &options->resolution)) {
                std::cerr << "resolution must be between 36 and 600 DPI\n";
                return false;
            }
        } else if (arg == "-q" || arg == "--webp-quality") {
            const char *value = requireValue(argv[i]);
            if (!value || !parseDouble(value, 0, 100, &options->webpQuality)) {
                std::cerr << "WebP quality must be between 0 and 100\n";
                return false;
            }
        } else if (arg == "-j" || arg == "--jobs") {
            const char *value = requireValue(argv[i]);
            if (!value || !parseInteger(value, &options->jobs) || options->jobs > 256) {
                std::cerr << "worker thread count must be between 1 and 256\n";
                return false;
            }
        } else if (arg == "--owner-password" || arg == "--user-password") {
            const bool owner = arg == "--owner-password";
            const char *value = requireValue(argv[i]);
            if (!value) {
                return false;
            }
            (owner ? options->ownerPassword : options->userPassword) = GooString(value);
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "unknown option: " << arg << '\n';
            return false;
        } else {
            positional.emplace_back(arg);
        }
    }
    if (positional.size() != 2) {
        return false;
    }
    options->input = std::move(positional[0]);
    options->output = std::move(positional[1]);
    return true;
}

std::string htmlEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
    return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) { return value.find(needle) != std::string_view::npos; });
}

std::string fontClasses(const TextWord *word)
{
    if (word->getLength() == 0) {
        return "f-sans";
    }
    const TextFontInfo *font = word->getFontInfo(0);
    std::string name;
    if (const GooString *fontName = font->getFontName()) {
        name = lower(fontName->toStr());
    }
    const bool mono = font->isFixedWidth() || containsAny(name, { "mono", "courier", "consolas", "code" });
    const bool sansByName = containsAny(name, { "sans", "arial", "helvetica", "verdana", "tahoma", "calibri", "gothic" });
    const bool serif = !mono && !sansByName && (font->isSerif() || containsAny(name, { "serif", "times", "georgia", "garamond", "cambria", "minion", "baskerville", "roman" }));
    const bool bold = font->isBold() || containsAny(name, { "bold", "black", "heavy", "semibold", "demi" });
    const bool italic = font->isItalic() || containsAny(name, { "italic", "oblique", "slanted" });
    std::string classes = mono ? "f-mono" : serif ? "f-serif" : "f-sans";
    if (bold) {
        classes += " fw-bold";
    }
    if (italic) {
        classes += " fs-italic";
    }
    return classes;
}

std::string base64Decode(std::string_view input)
{
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table {};
    table.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }
    std::string output;
    int accumulator = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        if (table[c] < 0) {
            return {};
        }
        accumulator = (accumulator << 6) | table[c];
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::string extensionForMime(std::string_view mime)
{
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/png") return "png";
    if (mime == "image/jp2") return "jp2";
    if (mime == "image/webp") return "webp";
    return "bin";
}

struct RgbaImage
{
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

cairo_status_t readPng(void *closure, unsigned char *data, unsigned int length)
{
    auto *input = static_cast<std::pair<std::string_view, std::size_t> *>(closure);
    if (length > input->first.size() - input->second) {
        return CAIRO_STATUS_READ_ERROR;
    }
    std::copy_n(reinterpret_cast<const unsigned char *>(input->first.data() + input->second), length, data);
    input->second += length;
    return CAIRO_STATUS_SUCCESS;
}

bool decodePng(std::string_view encoded, RgbaImage *image, std::string *error)
{
    std::pair<std::string_view, std::size_t> input(encoded, 0);
    cairo_surface_t *surface = cairo_image_surface_create_from_png_stream(readPng, &input);
    const cairo_status_t status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        *error = std::string("cannot decode PNG: ") + cairo_status_to_string(status);
        cairo_surface_destroy(surface);
        return false;
    }
    const cairo_format_t format = cairo_image_surface_get_format(surface);
    if (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24) {
        *error = "unsupported Cairo PNG pixel format";
        cairo_surface_destroy(surface);
        return false;
    }
    cairo_surface_flush(surface);
    image->width = cairo_image_surface_get_width(surface);
    image->height = cairo_image_surface_get_height(surface);
    if (image->width < 1 || image->height < 1 || image->width > WEBP_MAX_DIMENSION || image->height > WEBP_MAX_DIMENSION) {
        *error = "PNG dimensions are outside the WebP limit";
        cairo_surface_destroy(surface);
        return false;
    }
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char *source = cairo_image_surface_get_data(surface);
    image->pixels.resize(static_cast<std::size_t>(image->width) * image->height * 4);
    for (int y = 0; y < image->height; ++y) {
        const auto *row = reinterpret_cast<const std::uint32_t *>(source + static_cast<std::size_t>(y) * stride);
        for (int x = 0; x < image->width; ++x) {
            const std::uint32_t pixel = row[x];
            const unsigned int alpha = format == CAIRO_FORMAT_RGB24 ? 255 : pixel >> 24;
            unsigned int red = (pixel >> 16) & 0xff;
            unsigned int green = (pixel >> 8) & 0xff;
            unsigned int blue = pixel & 0xff;
            if (alpha != 0 && alpha != 255) {
                red = std::min(255u, (red * 255u + alpha / 2) / alpha);
                green = std::min(255u, (green * 255u + alpha / 2) / alpha);
                blue = std::min(255u, (blue * 255u + alpha / 2) / alpha);
            }
            const std::size_t destination = (static_cast<std::size_t>(y) * image->width + x) * 4;
            image->pixels[destination] = static_cast<unsigned char>(red);
            image->pixels[destination + 1] = static_cast<unsigned char>(green);
            image->pixels[destination + 2] = static_cast<unsigned char>(blue);
            image->pixels[destination + 3] = static_cast<unsigned char>(alpha);
        }
    }
    cairo_surface_destroy(surface);
    return true;
}

struct JpegError
{
    jpeg_error_mgr manager;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX] {};
};

void jpegErrorExit(j_common_ptr info)
{
    auto *jpegError = reinterpret_cast<JpegError *>(info->err);
    (*info->err->format_message)(info, jpegError->message);
    std::longjmp(jpegError->jump, 1);
}

bool decodeJpeg(std::string_view encoded, RgbaImage *image, std::string *error)
{
    jpeg_decompress_struct decoder {};
    JpegError jpegError {};
    decoder.err = jpeg_std_error(&jpegError.manager);
    jpegError.manager.error_exit = jpegErrorExit;
    if (setjmp(jpegError.jump)) {
        jpeg_destroy_decompress(&decoder);
        *error = std::string("cannot decode JPEG: ") + jpegError.message;
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size());
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    image->width = static_cast<int>(decoder.output_width);
    image->height = static_cast<int>(decoder.output_height);
    if (image->width < 1 || image->height < 1 || image->width > WEBP_MAX_DIMENSION || image->height > WEBP_MAX_DIMENSION) {
        jpeg_destroy_decompress(&decoder);
        *error = "JPEG dimensions are outside the WebP limit";
        return false;
    }
    image->pixels.resize(static_cast<std::size_t>(image->width) * image->height * 4);
    JSAMPARRAY row = (*decoder.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&decoder), JPOOL_IMAGE, decoder.output_width * decoder.output_components, 1);
    while (decoder.output_scanline < decoder.output_height) {
        jpeg_read_scanlines(&decoder, row, 1);
        const std::size_t y = decoder.output_scanline - 1;
        for (int x = 0; x < image->width; ++x) {
            const std::size_t source = static_cast<std::size_t>(x) * 3;
            const std::size_t destination = (y * image->width + x) * 4;
            std::copy_n(row[0] + source, 3, image->pixels.data() + destination);
            image->pixels[destination + 3] = 255;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return true;
}

bool convertToWebp(std::string_view mime, std::string_view encoded, double quality, std::vector<unsigned char> *webp, std::string *error)
{
    RgbaImage image;
    const bool decoded = mime == "image/png" ? decodePng(encoded, &image, error) : decodeJpeg(encoded, &image, error);
    if (!decoded) {
        return false;
    }
    unsigned char *encodedWebp = nullptr;
    const std::size_t size = WebPEncodeRGBA(image.pixels.data(), image.width, image.height, image.width * 4, static_cast<float>(quality), &encodedWebp);
    if (size == 0 || !encodedWebp) {
        *error = "cannot encode WebP image";
        return false;
    }
    webp->assign(encodedWebp, encodedWebp + size);
    WebPFree(encodedWebp);
    return true;
}

struct ExtractedAsset
{
    std::string token;
    std::string mime;
    std::vector<unsigned char> data;
};

bool extractDataUris(std::string *svg, int page, std::vector<ExtractedAsset> *assets, double webpQuality, std::string *error)
{
    std::size_t search = 0;
    while ((search = svg->find("data:", search)) != std::string::npos) {
        const std::size_t quote = svg->rfind('"', search);
        const std::size_t endQuote = svg->find('"', search);
        if (quote == std::string::npos || endQuote == std::string::npos) {
            *error = "malformed data URI in Cairo SVG";
            return false;
        }
        const std::size_t comma = svg->find(',', search);
        if (comma == std::string::npos || comma > endQuote) {
            *error = "malformed data URI header in Cairo SVG";
            return false;
        }
        const std::string metadata = svg->substr(search + 5, comma - search - 5);
        const std::size_t separator = metadata.find(';');
        const std::string mime = metadata.substr(0, separator);
        if (separator == std::string::npos || metadata.substr(separator + 1) != "base64" || !mime.starts_with("image/")) {
            *error = "unsupported non-base64 Cairo data URI";
            return false;
        }
        std::string decoded = base64Decode(std::string_view(*svg).substr(comma + 1, endQuote - comma - 1));
        if (decoded.empty()) {
            *error = "cannot decode Cairo image data URI";
            return false;
        }
        std::string outputMime = mime;
        std::vector<unsigned char> bytes;
        if (mime == "image/png" || mime == "image/jpeg") {
            if (!convertToWebp(mime, decoded, webpQuality, &bytes, error)) {
                return false;
            }
            outputMime = "image/webp";
        } else {
            bytes.assign(decoded.begin(), decoded.end());
        }
        const std::string token = "__pdfrender_page_" + std::to_string(page) + "_asset_" + std::to_string(assets->size()) + "__";
        const std::string replacement = token + "\" data-bundle-ref=\"" + token + "\" data-bundle-mime=\"" + outputMime;
        svg->replace(search, endQuote - search, replacement);
        search += replacement.size();
        assets->push_back({ token, std::move(outputMime), std::move(bytes) });
    }
    return true;
}

bool renderSvg(PDFDoc *doc, BackgroundOutputDev *output, int page, double resolution, std::string *svg, std::string *error)
{
    const double scale = resolution / 72.0;
    const double width = doc->getPageMediaWidth(page) * scale;
    const double height = doc->getPageMediaHeight(page) * scale;
    MemoryStream stream;
    cairo_surface_t *surface = cairo_svg_surface_create_for_stream(writeSvg, &stream, width, height);
    cairo_svg_surface_restrict_to_version(surface, CAIRO_SVG_VERSION_1_2);
#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 16, 0)
    cairo_svg_surface_set_document_unit(surface, CAIRO_SVG_UNIT_PX);
#endif
    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 1, 1, 1);
    cairo_paint(context);
    cairo_scale(context, scale, scale);
    output->setCairo(context);
    output->setPrinting(true);
    doc->displayPage(output, page, 72.0, 72.0, 0, true, false, false);
    output->setCairo(nullptr);
    const cairo_status_t contextStatus = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_finish(surface);
    const cairo_status_t surfaceStatus = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (contextStatus != CAIRO_STATUS_SUCCESS || surfaceStatus != CAIRO_STATUS_SUCCESS) {
        *error = cairo_status_to_string(contextStatus != CAIRO_STATUS_SUCCESS ? contextStatus : surfaceStatus);
        return false;
    }
    svg->assign(stream.data.begin(), stream.data.end());
    return true;
}

std::string makePageHtml(PDFDoc *doc, int page, double resolution, std::string_view cssId, std::string_view scriptId, std::string_view svg)
{
    HtmlTextOutputDev textOutput;
    doc->displayPage(&textOutput, page, resolution, resolution, 0, true, false, false);
    const auto words = textOutput.makeWordList();
    const double scale = resolution / 72.0;
    const int width = static_cast<int>(std::ceil(doc->getPageMediaWidth(page) * scale));
    const int height = static_cast<int>(std::ceil(doc->getPageMediaHeight(page) * scale));

    std::ostringstream html;
    html << std::fixed << std::setprecision(3);
    html << "<!doctype html><html><head><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         << "<link rel=\"stylesheet\" href=\"" << cssId << "\" data-bundle-ref=\"" << cssId << "\" data-bundle-mime=\"text/css\">"
         << "<script defer src=\"" << scriptId << "\" data-bundle-ref=\"" << scriptId << "\" data-bundle-mime=\"text/javascript\"></script>"
         << "</head><body><main class=\"page\" style=\"width:" << width << "px;height:" << height << "px\">"
         << "<div class=\"background\" aria-hidden=\"true\">" << svg << "</div>"
         << "<div class=\"text-layer\" aria-label=\"PDF text\">";
    for (const TextWord *word : words->getWords()) {
        const std::unique_ptr<std::string> text = word->getText();
        if (!text || text->empty()) {
            continue;
        }
        double x0, y0, x1, y1, red, green, blue;
        word->getBBox(&x0, &y0, &x1, &y1);
        word->getColor(&red, &green, &blue);
        html << "<span class=\"word " << fontClasses(word) << "\" style=\"left:" << x0 << "px;top:" << y0 << "px;width:" << std::max(0.0, x1 - x0) << "px;height:" << std::max(0.0, y1 - y0)
             << "px;font-size:" << word->getFontSize() << "px;color:rgb(" << std::lround(red * 255) << ',' << std::lround(green * 255) << ',' << std::lround(blue * 255) << ");";
        if (word->getRotation()) {
            html << "--rotation:" << word->getRotation() * 90 << "deg;";
        }
        html << "\">" << htmlEscape(*text) << "</span>";
    }
    html << "</div></main></body></html>";
    return html.str();
}

std::string pagePath(int page, std::string_view extension)
{
    std::ostringstream path;
    path << "pages/" << std::setfill('0') << std::setw(4) << page << extension;
    return path.str();
}

void normalizeSvgIds(std::string *svg, int page)
{
    std::unordered_map<std::string, std::string> ids;
    std::size_t search = 0;
    while ((search = svg->find("id=\"", search)) != std::string::npos) {
        const std::size_t value = search + 4;
        const std::size_t end = svg->find('"', value);
        if (end == std::string::npos) {
            break;
        }
        const std::string oldId = svg->substr(value, end - value);
        ids.try_emplace(oldId, "pdfrender-p" + std::to_string(page) + '-' + std::to_string(ids.size()));
        search = end + 1;
    }

    std::string normalized;
    normalized.reserve(svg->size());
    for (std::size_t position = 0; position < svg->size();) {
        if (svg->compare(position, 4, "id=\"") == 0) {
            const std::size_t value = position + 4;
            const std::size_t end = svg->find('"', value);
            if (end != std::string::npos) {
                const auto replacement = ids.find(svg->substr(value, end - value));
                if (replacement != ids.end()) {
                    normalized += "id=\"" + replacement->second + '"';
                    position = end + 1;
                    continue;
                }
            }
        } else if ((*svg)[position] == '#') {
            const std::size_t end = svg->find_first_of("\"') \t\r\n", position + 1);
            const std::size_t length = (end == std::string::npos ? svg->size() : end) - position - 1;
            const auto replacement = ids.find(svg->substr(position + 1, length));
            if (replacement != ids.end()) {
                normalized += '#' + replacement->second;
                position += length + 1;
                continue;
            }
        }
        normalized += (*svg)[position++];
    }
    *svg = std::move(normalized);
}

constexpr std::string_view styleSheet = R"CSS(*{box-sizing:border-box}html,body{margin:0;background:#555}.page{position:relative;overflow:hidden;background:#fff}.background,.text-layer{position:absolute;inset:0;width:100%;height:100%}.background{display:block;pointer-events:none}.background>svg{display:block;width:100%;height:100%}.text-layer{pointer-events:none}.word{position:absolute;display:block;white-space:pre;line-height:1;transform-origin:0 0;transform:rotate(var(--rotation,0deg)) scaleX(var(--fit-x,1));pointer-events:auto;user-select:text}.f-sans{font-family:Arial,"Helvetica Neue",sans-serif}.f-serif{font-family:"Times New Roman",Times,serif}.f-mono{font-family:Consolas,"Liberation Mono",monospace}.fw-bold{font-weight:700}.fs-italic{font-style:italic}body.bundle-index{padding:1rem}.bundle-index iframe{display:block;margin:0 auto 1rem;border:0;background:#fff;box-shadow:0 2px 10px #222})CSS";

constexpr std::string_view fitScript = R"JS((()=>{const fit=()=>{for(const word of document.querySelectorAll('.word')){const target=Number.parseFloat(word.style.width);if(!(target>0)||!word.textContent||word.textContent.length<2)continue;word.style.transform='none';const range=document.createRange();range.selectNodeContents(word);const natural=range.getBoundingClientRect().width;word.style.removeProperty('transform');if(natural>0){const ratio=Math.max(.5,Math.min(2,target/natural));word.style.setProperty('--fit-x',String(ratio))}}};if(document.fonts&&document.fonts.ready){document.fonts.ready.then(fit)}else if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',fit,{once:true})}else{fit()}})();)JS";

struct PageResult
{
    std::string html;
    std::vector<ExtractedAsset> assets;
    std::string error;
};

bool renderPage(PDFDoc *document, BackgroundOutputDev *background, int page, const Options &options, std::string_view cssId, std::string_view scriptId, PageResult *result)
{
    std::string svg;
    if (!renderSvg(document, background, page, options.resolution, &svg, &result->error) || !extractDataUris(&svg, page, &result->assets, options.webpQuality, &result->error)) {
        return false;
    }
    const std::size_t root = svg.find("<svg");
    if (root == std::string::npos) {
        result->error = "Cairo output does not contain an SVG root element";
        return false;
    }
    svg.erase(0, root);
    normalizeSvgIds(&svg, page);
    result->html = makePageHtml(document, page, options.resolution, cssId, scriptId, svg);
    return true;
}

void replaceAll(std::string *value, std::string_view from, std::string_view to)
{
    std::size_t position = 0;
    while ((position = value->find(from, position)) != std::string::npos) {
        value->replace(position, from.size(), to);
        position += to.size();
    }
}

std::vector<std::string> materializeAssets(PageResult *page, PdfRenderBundle *bundle)
{
    std::vector<std::string> ids;
    ids.reserve(page->assets.size());
    for (auto &asset : page->assets) {
        // First add obtains the content id; adding the canonical path then
        const std::string id = bundle->addFile({}, asset.mime, std::move(asset.data));
        if (id.empty() || !bundle->addPath("assets/" + id + "." + extensionForMime(asset.mime), id)) {
            return {};
        }
        replaceAll(&page->html, asset.token, id);
        ids.push_back(id);
    }
    return ids;
}

} // namespace

int main(int argc, char **argv)
{
    Win32Console win32Console(&argc, &argv);
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    globalParams = std::make_unique<GlobalParams>();
    globalParams->setTextEncoding("UTF-8");
    const GooString input(options.input);
    std::unique_ptr<PDFDoc> document = PDFDocFactory().createPDFDoc(input, options.ownerPassword, options.userPassword);
    if (!document || !document->isOk()) {
        std::cerr << "cannot open PDF: " << options.input << '\n';
        return 1;
    }
    if (!document->okToCopy()) {
        std::cerr << "PDF permissions do not allow text extraction\n";
        return 1;
    }
    const int pageCount = document->getNumPages();
    if (pageCount < 1) {
        std::cerr << "PDF contains no pages\n";
        return 1;
    }
    const int first = std::clamp(options.firstPage, 1, pageCount);
    const int last = options.lastPage == 0 ? pageCount : std::clamp(options.lastPage, first, pageCount);

    PdfRenderBundle bundle;
    const std::string cssId = bundle.addText("style.css", "text/css", std::string(styleSheet));
    const std::string scriptId = bundle.addText("pdfrender.js", "text/javascript", std::string(fitScript));
    if (cssId.empty() || scriptId.empty()) {
        std::cerr << "cannot spool shared render resources\n";
        return 1;
    }
    const int resultCount = last - first + 1;
    const unsigned int detectedThreads = std::clamp(std::thread::hardware_concurrency(), 1u, 256u);
    const int requestedThreads = options.jobs == 0 ? static_cast<int>(detectedThreads) : options.jobs;
    const int workerCount = std::min(resultCount, requestedThreads);
    struct ManifestPage
    {
        int page = 0;
        int width = 0;
        int height = 0;
        std::string htmlId;
        std::vector<std::string> dependencies;
    };
    std::vector<ManifestPage> pages(resultCount);
    std::atomic<int> nextResult { 0 };
    std::atomic<bool> failed { false };
    std::mutex failureMutex;
    std::mutex bundleMutex;
    std::string workerFailure;

    auto worker = [&] {
        const GooString workerInput(options.input);
        std::unique_ptr<PDFDoc> workerDocument = PDFDocFactory().createPDFDoc(workerInput, options.ownerPassword, options.userPassword);
        if (!workerDocument || !workerDocument->isOk()) {
            std::scoped_lock lock(failureMutex);
            if (workerFailure.empty()) {
                workerFailure = "cannot open PDF in worker thread";
            }
            failed = true;
            return;
        }
        BackgroundOutputDev background;
        background.startDoc(workerDocument.get());
        while (!failed.load(std::memory_order_relaxed)) {
            const int resultIndex = nextResult.fetch_add(1, std::memory_order_relaxed);
            if (resultIndex >= resultCount) {
                break;
            }
            const int page = first + resultIndex;
            PageResult result;
            if (!renderPage(workerDocument.get(), &background, page, options, cssId, scriptId, &result)) {
                std::scoped_lock lock(failureMutex);
                if (workerFailure.empty()) {
                    workerFailure = "page " + std::to_string(page) + ": " + result.error;
                }
                failed = true;
                break;
            }
            std::scoped_lock bundleLock(bundleMutex);
            const std::size_t assetCount = result.assets.size();
            std::vector<std::string> dependencies = materializeAssets(&result, &bundle);
            const std::string htmlId = dependencies.size() == assetCount ? bundle.addText(pagePath(page, ".html"), "text/html", std::move(result.html)) : std::string {};
            if (htmlId.empty()) {
                std::scoped_lock lock(failureMutex);
                if (workerFailure.empty()) {
                    workerFailure = "page " + std::to_string(page) + ": cannot spool render resources";
                }
                failed = true;
                break;
            }
            const int width = static_cast<int>(std::ceil(workerDocument->getPageMediaWidth(page) * options.resolution / 72.0));
            const int height = static_cast<int>(std::ceil(workerDocument->getPageMediaHeight(page) * options.resolution / 72.0));
            pages[resultIndex] = { page, width, height, htmlId, std::move(dependencies) };
        }
    };

    {
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i) {
            workers.emplace_back(worker);
        }
    }
    if (failed) {
        std::cerr << workerFailure << '\n';
        return 1;
    }

    std::string error;
    std::ostringstream manifest;
    manifest << "{\"layout\":\"fixed\",\"sourceMime\":\"application/pdf\",\"sourcePages\":" << pageCount
             << ",\"firstPage\":" << first << ",\"lastPage\":" << last << ",\"resolution\":" << options.resolution
             << ",\"webpQuality\":" << options.webpQuality << ",\"shared\":[\"" << cssId << "\",\"" << scriptId << "\"],\"items\":[";
    for (std::size_t index = 0; index < pages.size(); ++index) {
        const auto &page = pages[index];
        if (index) manifest << ',';
        manifest << "{\"id\":\"page:" << page.page << "\",\"ordinal\":" << index << ",\"width\":" << page.width
                 << ",\"height\":" << page.height << ",\"document\":\"" << page.htmlId << "\",\"dependencies\":[\"" << cssId << "\",\"" << scriptId << '"';
        for (const auto &dependency : page.dependencies) {
            manifest << ",\"" << dependency << '"';
        }
        manifest << "]}";
    }
    manifest << "]}";
    bundle.setDocumentManifest(manifest.str());
    if (!bundle.write(options.output, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "wrote " << options.output << " (pages " << first << '-' << last << ", threads " << workerCount << ")\n";
    return 0;
}
