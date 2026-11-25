#pragma once
#include <string>

// Arabic text in the form the game's own Arabic strings have (its locals.bin, Arabic copy): shaped already and in display
// order, as the game's text fields show them as they are (left to right, no shaping of their own). From the strings:
//   - the letters that join on both sides, teh marbuta, alef maksura and yeh with hamza: their Arabic Presentation
//     Forms-B form (isolated, final, initial, medial: the one the neighbours make);
//   - the alefs, dal, thal, reh, zain, waw, hamza, and an isolated heh: the plain letter (what the game's strings use
//     most for them, in every form);
//   - no lam-alef ligature (lam's initial or medial form, then the alef);
//   - the line from right to left: the Arabic reversed, a run of Latin letters or digits kept in its own order (the
//     punctuation at the line's end goes to the left, as in the game's strings).
// Checked against the game's strings: of its 6111 Arabic strings without Latin letters, digits or markup, turned back
// into reading order and plain letters, this gives 2636 back exactly; the others differ only where the game's strings
// themselves use a letter's form instead of the plain letter (the alefs, dal, reh, waw, zain, thal; a handful of others).
namespace ArabicDisplay {

// UTF-8 in, UTF-8 out; text without Arabic letters comes back unchanged.
inline std::string FromLogical(const std::string& utf8) {
    std::u32string text;
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        char32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) cp = c & 0x07, extra = 3;
        else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
        else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        text.push_back(cp);
    }

    // letter -> its isolated, final, initial, medial code points (0: the letter itself; the initial and medial of a
    // letter that joins only the one before it are 0 too)
    struct Forms {
        char32_t letter, isolated, final, initial, medial;
    };
    static constexpr Forms kForms[] = {
        {0x0621, 0, 0, 0, 0},                                  // hamza (joins nothing)
        {0x0622, 0, 0, 0, 0},      {0x0623, 0, 0, 0, 0},       {0x0624, 0, 0, 0, 0},       {0x0625, 0, 0, 0, 0},
        {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},              {0x0627, 0, 0, 0, 0},
        {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},              {0x0629, 0xFE93, 0xFE94, 0, 0},
        {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},              {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},
        {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},              {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},
        {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},              {0x062F, 0, 0, 0, 0},       {0x0630, 0, 0, 0, 0},
        {0x0631, 0, 0, 0, 0},      {0x0632, 0, 0, 0, 0},       {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},
        {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},              {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
        {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},              {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},
        {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},              {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},
        {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},              {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
        {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},              {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
        {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},              {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},
        {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},              {0x0647, 0, 0xFEEA, 0xFEEB, 0xFEEC},
        {0x0648, 0, 0, 0, 0},      {0x0649, 0xFEEF, 0xFEF0, 0, 0},                         {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    };
    // how it joins: 0 not at all, 1 to the letter before it only, 2 on both sides
    auto joining = [](char32_t c) -> int {
        switch (c) {
            case 0x0621: return 0;
            case 0x0622: case 0x0623: case 0x0624: case 0x0625: case 0x0627: case 0x0629: case 0x062F: case 0x0630:
            case 0x0631: case 0x0632: case 0x0648: case 0x0649: return 1;
            default: return (c >= 0x0626 && c <= 0x063A) || (c >= 0x0641 && c <= 0x064A) ? 2 : -1;   // -1: not a letter
        }
    };
    auto forms = [](char32_t c) -> const Forms* {
        for (const Forms& f : kForms)
            if (f.letter == c) return &f;
        return nullptr;
    };

    bool any = false;
    std::u32string shaped(text.size(), 0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t c = text[i];
        const Forms* f = forms(c);
        if (!f) {
            shaped[i] = c;
            continue;
        }
        any = true;
        const int self = joining(c);
        const bool withBefore = self >= 1 && i > 0 && joining(text[i - 1]) == 2;
        const bool withAfter = self == 2 && i + 1 < text.size() && joining(text[i + 1]) >= 1;
        const char32_t form = withBefore ? (withAfter ? f->medial : f->final) : (withAfter ? f->initial : f->isolated);
        shaped[i] = form ? form : c;
    }
    if (!any) return utf8;

    // display order: runs of Latin letters and digits (with the spaces and the few marks between their words) keep their
    // order; the rest is reversed, and so is the order of the runs
    auto latin = [](char32_t c) { return c < 0x80 && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')); };
    auto joinsLatin = [](char32_t c) { return c == ' ' || c == ':' || c == '-' || c == '\''; };
    std::u32string display;
    std::size_t end = shaped.size();
    while (end > 0) {
        std::size_t start = end;
        if (latin(shaped[end - 1])) {
            while (start > 0 && (latin(shaped[start - 1]) ||
                                 (joinsLatin(shaped[start - 1]) && start >= 2 && latin(shaped[start - 2]))))
                --start;
            display.append(shaped, start, end - start);
        } else {
            while (start > 0 && !latin(shaped[start - 1])) --start;
            for (std::size_t k = end; k > start; --k) display.push_back(shaped[k - 1]);
        }
        end = start;
    }

    std::string out;
    for (char32_t cp : display) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

}  // namespace ArabicDisplay
