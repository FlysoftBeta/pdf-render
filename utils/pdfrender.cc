// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include "PdfRenderBundle.h"
#include "goo/GooString.h"
#include "GlobalParams.h"
#include "GfxState.h"
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
#include <filesystem>
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

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

// Win32Console.h remaps stdio functions with macros. Include it only after
// headers that may declare methods with those names (notably Stream::printf).
#include "Win32Console.h"

namespace {

enum class MathFontKind
{
    none,
    roman,
    italic,
    symbol,
};

MathFontKind classifyMathFontName(std::string_view value)
{
    std::string name(value);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto contains = [&](std::initializer_list<std::string_view> needles) { return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) { return name.find(needle) != std::string::npos; }); };
    if (contains({ "cmsy", "cmex", "msam", "msbm", "mathsymbol", "math-symbol", "mathematicalpi", "mt extra", "wasy", "stmary", "esint" })) {
        return MathFontKind::symbol;
    }
    if (contains({ "cmmi", "cmmib", "mathitalic", "math-italic", "eufm", "rsfs" })) {
        return MathFontKind::italic;
    }
    if (contains({ "cmr", "cmbx", "mathroman", "math-roman", "latinmodernmath", "latin modern math", "stixmath", "stix math", "xits math", "asana math", "cambria math", "libertinus math", "computer modern", "euclid" })) {
        return MathFontKind::roman;
    }
    return MathFontKind::none;
}

std::filesystem::path executablePath(const char *argv0)
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
#elif defined(__linux__)
    std::error_code procError;
    const std::filesystem::path procPath = std::filesystem::read_symlink("/proc/self/exe", procError);
    if (!procError) {
        return procPath;
    }
#endif

    std::error_code canonicalError;
    const std::filesystem::path path = std::filesystem::weakly_canonical(std::filesystem::absolute(argv0), canonicalError);
    return canonicalError ? std::filesystem::absolute(argv0) : path;
}

std::optional<std::string> bundledPopplerDataDir(const char *argv0)
{
    const std::filesystem::path root = executablePath(argv0).parent_path() / "share" / "poppler";
    for (const char *directory : { "nameToUnicode", "cidToUnicode", "unicodeMap", "cMap" }) {
        if (!std::filesystem::is_directory(root / directory)) {
            std::cerr << "bundled poppler-data is incomplete: missing " << (root / directory).string() << '\n';
            return std::nullopt;
        }
    }
    const std::u8string encoded = root.u8string();
    return std::string(encoded.begin(), encoded.end());
}

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

    // Ordinary text is represented by positioned HTML. Math fonts retain
    // their original PDF glyphs in SVG because radicals, extensible symbols,
    // and script placement cannot be reproduced reliably with CSS fallback
    // fonts. A transparent HTML copy remains selectable and searchable.
    void beginString(GfxState *state, const std::string &text) override
    {
        renderMath = false;
        if (const std::shared_ptr<GfxFont> &font = state->getFont(); font && font->getName()) {
            renderMath = classifyMathFontName(*font->getName()) != MathFontKind::none;
        }
        if (renderMath) {
            CairoOutputDev::beginString(state, text);
        }
    }
    void drawChar(GfxState *state, double x, double y, double dx, double dy, double originX, double originY, CharCode code, int bytes, const Unicode *unicode, int unicodeLength) override
    {
        if (renderMath) {
            CairoOutputDev::drawChar(state, x, y, dx, dy, originX, originY, code, bytes, unicode, unicodeLength);
        }
    }
    void endString(GfxState *state) override
    {
        if (renderMath) {
            CairoOutputDev::endString(state);
        }
        renderMath = false;
    }

private:
    bool renderMath = false;
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

bool isMathSymbol(Unicode codepoint)
{
    return codepoint == 0x00b1 || codepoint == 0x00d7 || codepoint == 0x00f7 || codepoint == 0x2032 || codepoint == 0x2033 || (codepoint >= 0x2070 && codepoint <= 0x209f) || (codepoint >= 0x2190 && codepoint <= 0x22ff)
            || (codepoint >= 0x27c0 && codepoint <= 0x27ef) || (codepoint >= 0x2980 && codepoint <= 0x2aff);
}

bool containsMathSymbol(const TextWord *word)
{
    for (int i = 0; i < word->getLength(); ++i) {
        if (isMathSymbol(*word->getChar(i))) {
            return true;
        }
    }
    return false;
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
    const MathFontKind mathKind = classifyMathFontName(name);
    const bool mathItalic = mathKind == MathFontKind::italic;
    const bool mathSymbol = containsMathSymbol(word) || mathKind == MathFontKind::symbol;
    const bool mathRoman = mathKind == MathFontKind::roman;
    if (mathItalic || mathSymbol || mathRoman) {
        std::string classes = mathSymbol ? "f-math-symbol" : "f-math";
        if (mathKind != MathFontKind::none) {
            classes += " pdf-glyph";
        }
        if (font->isBold() || containsAny(name, { "bold", "black", "heavy", "semibold", "demi", "cmbx", "cmmib" })) {
            classes += " fw-bold";
        }
        // Symbol and extension fonts often advertise an italic flag even for
        // upright operators, radicals, and delimiters. Only synthesize italic
        // for fonts that explicitly identify themselves as math italic.
        if (mathItalic) {
            classes += " fs-italic";
        }
        return classes;
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

struct PositionedWord
{
    const TextWord *word;
    double x0;
    double y0;
    double x1;
    double y1;
    double fontSize;
};

struct TextLineGroup
{
    std::vector<PositionedWord> words;
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    double centerY = 0;
    double fontSize = 0;
};

struct ParagraphGroup
{
    std::vector<TextLineGroup> lines;
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

void updateLineGeometry(TextLineGroup *line)
{
    line->x0 = line->x1 = line->words.front().x0;
    line->y0 = line->y1 = line->words.front().y0;
    double fontSizeSum = 0;
    double weightedCenterSum = 0;
    double weightSum = 0;
    for (const PositionedWord &word : line->words) {
        line->x0 = std::min(line->x0, word.x0);
        line->y0 = std::min(line->y0, word.y0);
        line->x1 = std::max(line->x1, word.x1);
        line->y1 = std::max(line->y1, word.y1);
        const double weight = std::max(1.0, word.x1 - word.x0);
        weightedCenterSum += (word.y0 + word.y1) * 0.5 * weight;
        weightSum += weight;
        fontSizeSum += word.fontSize;
    }
    line->centerY = weightedCenterSum / weightSum;
    line->fontSize = fontSizeSum / line->words.size();
}

std::vector<ParagraphGroup> groupParagraphs(const TextWordList &wordList)
{
    std::vector<PositionedWord> positioned;
    positioned.reserve(wordList.getWords().size());
    for (const TextWord *word : wordList.getWords()) {
        const std::unique_ptr<std::string> text = word->getText();
        if (!text || text->empty()) {
            continue;
        }
        double x0, y0, x1, y1;
        word->getBBox(&x0, &y0, &x1, &y1);
        positioned.push_back({ word, x0, y0, x1, y1, word->getFontSize() });
    }
    std::sort(positioned.begin(), positioned.end(), [](const PositionedWord &a, const PositionedWord &b) {
        const double ay = (a.y0 + a.y1) * 0.5;
        const double by = (b.y0 + b.y1) * 0.5;
        return ay == by ? a.x0 < b.x0 : ay < by;
    });

    // PDF text operators have no paragraph concept. First recover visual
    // lines by assigning each word to the nearest compatible vertical band.
    // The relaxed center threshold keeps roots, limits, and superscripts on
    // the same line without joining normal adjacent body lines.
    std::vector<TextLineGroup> lines;
    for (const PositionedWord &word : positioned) {
        const double center = (word.y0 + word.y1) * 0.5;
        std::size_t best = lines.size();
        double bestDistance = 1e100;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const double distance = std::abs(center - lines[i].centerY);
            const double tolerance = std::max(3.0, 0.68 * std::max(word.fontSize, lines[i].fontSize));
            if (distance <= tolerance && distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        if (best == lines.size()) {
            lines.push_back({ { word } });
            updateLineGeometry(&lines.back());
        } else {
            lines[best].words.push_back(word);
            updateLineGeometry(&lines[best]);
        }
    }
    for (TextLineGroup &line : lines) {
        std::sort(line.words.begin(), line.words.end(), [](const PositionedWord &a, const PositionedWord &b) { return a.x0 < b.x0; });
        updateLineGeometry(&line);
    }
    std::vector<TextLineGroup> splitLines;
    for (TextLineGroup &line : lines) {
        TextLineGroup segment;
        for (const PositionedWord &word : line.words) {
            if (!segment.words.empty()) {
                const double gap = word.x0 - segment.x1;
                const double splitGap = std::max(32.0, 4.0 * std::max(word.fontSize, segment.fontSize));
                if (gap > splitGap) {
                    splitLines.push_back(std::move(segment));
                    segment = TextLineGroup {};
                }
            }
            segment.words.push_back(word);
            updateLineGeometry(&segment);
        }
        if (!segment.words.empty()) {
            splitLines.push_back(std::move(segment));
        }
    }
    lines = std::move(splitLines);
    std::sort(lines.begin(), lines.end(), [](const TextLineGroup &a, const TextLineGroup &b) { return a.centerY == b.centerY ? a.x0 < b.x0 : a.centerY < b.centerY; });

    // Grow independent vertical chains. A line continues a paragraph only
    // when it is close to, and horizontally aligned with, that paragraph's
    // previous line. This keeps columns, headers, captions, and display labels
    // separate while allowing indentation in ordinary prose.
    std::vector<ParagraphGroup> paragraphs;
    for (TextLineGroup &line : lines) {
        std::size_t best = paragraphs.size();
        double bestGap = 1e100;
        for (std::size_t i = 0; i < paragraphs.size(); ++i) {
            const TextLineGroup &previous = paragraphs[i].lines.back();
            const double centerGap = line.centerY - previous.centerY;
            const double maxGap = 1.9 * std::max(line.fontSize, previous.fontSize);
            if (centerGap <= 0 || centerGap > maxGap) {
                continue;
            }
            const double overlap = std::max(0.0, std::min(line.x1, previous.x1) - std::max(line.x0, previous.x0));
            const double shorterWidth = std::max(1.0, std::min(line.x1 - line.x0, previous.x1 - previous.x0));
            const bool aligned = overlap / shorterWidth >= 0.2 || std::abs(line.x0 - previous.x0) <= 2.2 * std::max(line.fontSize, previous.fontSize);
            if (aligned && centerGap < bestGap) {
                best = i;
                bestGap = centerGap;
            }
        }
        if (best == paragraphs.size()) {
            ParagraphGroup paragraph;
            paragraph.x0 = line.x0;
            paragraph.y0 = line.y0;
            paragraph.x1 = line.x1;
            paragraph.y1 = line.y1;
            paragraph.lines.push_back(std::move(line));
            paragraphs.push_back(std::move(paragraph));
        } else {
            ParagraphGroup &paragraph = paragraphs[best];
            paragraph.x0 = std::min(paragraph.x0, line.x0);
            paragraph.y0 = std::min(paragraph.y0, line.y0);
            paragraph.x1 = std::max(paragraph.x1, line.x1);
            paragraph.y1 = std::max(paragraph.y1, line.y1);
            paragraph.lines.push_back(std::move(line));
        }
    }
    std::sort(paragraphs.begin(), paragraphs.end(), [](const ParagraphGroup &a, const ParagraphGroup &b) {
        const long aRow = std::lround(a.y0 / 4.0);
        const long bRow = std::lround(b.y0 / 4.0);
        return aRow == bRow ? a.x0 < b.x0 : aRow < bRow;
    });
    return paragraphs;
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
    const std::vector<ParagraphGroup> paragraphs = groupParagraphs(*words);
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
    for (std::size_t paragraphIndex = 0; paragraphIndex < paragraphs.size(); ++paragraphIndex) {
        const ParagraphGroup &paragraph = paragraphs[paragraphIndex];
        html << "<p class=\"paragraph\" data-paragraph=\"" << paragraphIndex << "\" style=\"left:" << paragraph.x0 << "px;top:" << paragraph.y0 << "px;width:" << std::max(0.0, paragraph.x1 - paragraph.x0) << "px;height:"
             << std::max(0.0, paragraph.y1 - paragraph.y0) << "px;\">";
        for (std::size_t lineIndex = 0; lineIndex < paragraph.lines.size(); ++lineIndex) {
            const TextLineGroup &line = paragraph.lines[lineIndex];
            const double bandTop = lineIndex == 0 ? paragraph.y0 : (paragraph.lines[lineIndex - 1].y1 + line.y0) * 0.5;
            const double bandBottom = lineIndex + 1 == paragraph.lines.size() ? paragraph.y1 : (line.y1 + paragraph.lines[lineIndex + 1].y0) * 0.5;
            html << "<span class=\"line\" data-line=\"" << lineIndex << "\" style=\"left:" << line.x0 - paragraph.x0 << "px;top:" << bandTop - paragraph.y0 << "px;width:" << std::max(0.0, line.x1 - line.x0) << "px;height:" << std::max(0.0, bandBottom - bandTop) << "px;\">";
            for (const PositionedWord &positioned : line.words) {
                const TextWord *word = positioned.word;
                const std::unique_ptr<std::string> text = word->getText();
                double red, green, blue;
                word->getColor(&red, &green, &blue);
                html << "<span class=\"word " << fontClasses(word) << "\" style=\"left:" << positioned.x0 - line.x0 << "px;top:" << positioned.y0 - bandTop << "px;width:" << std::max(0.0, positioned.x1 - positioned.x0) << "px;height:"
                     << std::max(0.0, positioned.y1 - positioned.y0) << "px;font-size:" << positioned.fontSize << "px;color:rgb(" << std::lround(red * 255) << ',' << std::lround(green * 255) << ',' << std::lround(blue * 255) << ");";
                if (word->getRotation()) {
                    html << "--rotation:" << word->getRotation() * 90 << "deg;";
                }
                html << "\">" << htmlEscape(*text) << "</span>";
            }
            html << "</span>";
        }
        html << "</p>";
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

constexpr std::string_view styleSheet = R"CSS(*{box-sizing:border-box}html,body{margin:0;background:#555}.page{position:relative;overflow:hidden;background:#fff}.background,.text-layer{position:absolute;inset:0;width:100%;height:100%}.background{display:block}.background>svg{display:block;width:100%;height:100%}.paragraph,.line,.word{position:absolute}.paragraph{display:block;margin:0}.line{display:block}.word{display:block;white-space:pre;line-height:1;transform-origin:0 0;transform:rotate(var(--rotation,0deg)) scaleX(var(--fit-x,1))}.f-sans{font-family:Arial,"Helvetica Neue",sans-serif}.f-serif{font-family:"Times New Roman",Times,serif}.f-mono{font-family:Consolas,"Liberation Mono",monospace}.f-math{font-family:"STIX Two Text",Cambria,"Times New Roman",serif}.f-math-symbol{font-family:"STIX Two Math","Cambria Math","Latin Modern Math","Noto Sans Math",serif;font-style:normal}.pdf-glyph{color:transparent!important}.fw-bold{font-weight:700}.fs-italic{font-style:italic}body.bundle-index{padding:1rem}.bundle-index iframe{display:block;margin:0 auto 1rem;border:0;background:#fff;box-shadow:0 2px 10px #222})CSS";

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

    const std::optional<std::string> popplerDataDir = bundledPopplerDataDir(argv[0]);
    if (!popplerDataDir) {
        return 1;
    }
    globalParams = std::make_unique<GlobalParams>(*popplerDataDir);
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
