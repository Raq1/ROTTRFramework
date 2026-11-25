#pragma once
#include <map>
#include <string>

// The Extras menus' text (the main menu's EXTRAS, the Extras menu, the Cutscene Viewer screen) in the game's text
// language: the game's own strings where it has the same thing (localstr_get by their ID: EXTRAS, CREDITS), our
// translations for the rest, in the 15 text languages the game's locals.bin has (English, French,
// German, Italian, Latin American and Iberian Spanish, Japanese, Brazilian Portuguese, Polish, Russian, Dutch, Korean,
// Traditional and Simplified Chinese, Arabic; the Arabic in the game's form: arabic_display.h). The texts are UTF-8
// and stay valid for the session. Game thread.
namespace ViewerText {
enum Id {
    Extras,
    CutsceneViewer,
    ModelViewer,
    Credits,
    AboutCutsceneViewer,
    AboutModelViewer,
    AboutCredits,
    Cutscene,
    Outfit,
    Play,
    StoryOutfit,
    NoneYet,
    NoCutscenes,
    Outfits,
    Characters,
    Animals,
    Weapons,
    Relics,
    AllUpgrades,   // (v0.9.438) the Model Viewer's grid: the weapons with every upgrade
    Search,        // the outfits' search
    Count
};
const char* Get(Id id);
const char* Upper(Id id);                 // in capitals (the menus' labels)
std::string ToUpper(const std::string& utf8);
const char* LanguageName();               // the game's text language now (for the log)
std::string GameText(int id);             // the game's string of that ID in its text language ("": none)
const char* LanguageCode();               // its code (localstr_get_language_code's: en, fr, ..., cht, chs, ar)
// The text of the game's language in a map by language code (cutscenes.json "titles", "descriptions"), else fallback;
// Arabic in the game's form (arabic_display.h).
std::string Localized(const std::map<std::string, std::string>& texts, const std::string& fallback);
// (v0.9.447) Our text of the game's language from one per language, in LanguageCode's order (en, fr, de, it, es-mx,
// es, ja, pt, pl, ru, nl, ko, cht, chs, ar; a null one: the English, texts[0]); Arabic in the game's form.
constexpr int kLanguageCount = 15;
std::string Pick(const char* const (&texts)[kLanguageCount]);
}  // namespace ViewerText
