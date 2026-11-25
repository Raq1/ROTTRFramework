#pragma once
#include <map>
#include <string>

// The cutscene viewer's titles in the game's other text languages (cutscenes.json "titles": the game's language code,
// as localstr_get_language_code names it, -> the title). Built in for the cutscenes the viewer lists; written into
// cutscenes.json while an entry has no "titles" and its title is still the English one these translate.
namespace CutsceneTitles {
// The translations of a cutscene's title (fr, de, it, es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, ar; the Arabic in
// its reading order), or null: none built in, or its title is no longer that English one.
const std::map<std::string, std::string>* Translations(const std::string& id, const std::string& englishTitle);
}  // namespace CutsceneTitles
