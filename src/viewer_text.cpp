// UTF-8 source (built with /utf-8: its strings are UTF-8, as Scaleform takes them).
#include "viewer_text.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#include "arabic_display.h"
#include "game_build.h"
#include "log.h"

namespace ViewerText {
namespace {
// The game's text languages (language_t, as localstr_get_name / localstr_get_language_code name them: TR2 0x95CA0 /
// 0x95A60): 0 english, 1 french, 2 german, 3 italian, 4 latamspanish (es-mx), 5 iberspanish (es), 6 japanese,
// 7 portuguese, 8 polish, 9 russian, 10 dutch, 11 korean, 12 chinese (cht), 13 simplechinese (chs), 14 ksa_english,
// 15 arabic, 16 czech, 17 spanish, 18 hungarian, 19 ukenglish. update3's locals.bin has a copy for each of 0 to 15
// (its specialisation bits; 14 is the English text); our columns: en, fr, de, it, es-MX, es-ES, ja, pt-BR, pl, ru, nl,
// ko, zh-TW, zh-CN, ar. Czech and Hungarian (no text) and the English variants read English.
constexpr int kColumns = 15;
constexpr int kArabicColumn = 14;
int Column(int language) {
    switch (language) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: case 17: return 5;
        case 6: return 6;
        case 7: return 7;
        case 8: return 8;
        case 9: return 9;
        case 10: return 10;
        case 11: return 11;
        case 12: return 12;
        case 13: return 13;
        case 15: return kArabicColumn;
        default: return 0;
    }
}
// The game's language codes for our columns (localstr_get_language_code, retail 0x833B0: its table names en, fr, de,
// it, es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, en, ar, cs, es, hu, en for language_t 0 to 19): cutscenes.json's
// "titles" and "descriptions" use them.
const char* const kCodes[kColumns] = {"en", "fr", "de", "it", "es-mx", "es", "ja", "pt", "pl", "ru", "nl", "ko",
                                      "cht", "chs", "ar"};
const char* const kLanguageNames[] = {"English", "French", "German", "Italian", "Latin American Spanish",
                                      "Iberian Spanish", "Japanese", "Portuguese", "Polish", "Russian", "Dutch",
                                      "Korean", "Traditional Chinese", "Simplified Chinese", "English (KSA)", "Arabic",
                                      "Czech", "Spanish", "Hungarian", "English (UK)"};

// The game's own string where it has the same thing (its ID in locals.bin, the same in every language's copy; its
// English text checked when the game is in English), else ours. IDs from update3's locals.bin (7424 strings): 2786
// EXTRAS and 2561 CREDITS (Tomb Raider 2013's extras: STATISTICS, EQUIPPED WEAPONS, EXTRAS; CHARACTER MODELS, ...,
// CREDITS). Ours use the game's words: its cinematics (2194 "Use ... to adjust cinematics volume": cinématiques,
// Filmsequenz, filmati, cinemáticas / escenas, ムービー, animações, sekwencje filmowe, ролики, tussenfilms, 시네마틱,
// 影片, المشاهد السينمائية), outfits (2924 OUTFITS: tenues, Outfits, completi, atuendos, コスチューム, roupas, stroje,
// костюмы, 복장, 裝束 / 装束, الأزياء) and story (2746 Story Objective). Not the game's PLAY (2757: the audio logs'
// player, "ASCOLTA" in Italian, "ИГРАТЬ" in Russian), Outfit (3271: "Completi", "Roupas", "Снаряжение") or All (3168:
// a filter's, "Tutto", "Tudo", "Wszystko"): ours fit a cutscene. The Arabic is written in its reading order and made
// into the game's form (ArabicDisplay) when used.
struct Entry {
    int gameId;
    const char* english;
    const char* text[kColumns];
};
const Entry kEntries[Count] = {
    // Extras
    {2786, "EXTRAS",
     {"EXTRAS", "BONUS", "EXTRAS", "EXTRA", "EXTRAS", "EXTRAS", "エクストラ", "EXTRAS", "DODATKI", "ДОПОЛНЕНИЯ",
      "EXTRA'S", "부가 요소", "特別收錄", "花絮", "الإضافات"}},
    // CutsceneViewer
    {-1, nullptr,
     {"CUTSCENE VIEWER", "VISIONNEUSE DE CINÉMATIQUES", "FILMSEQUENZ-BETRACHTER", "VISUALIZZATORE FILMATI",
      "VISOR DE CINEMÁTICAS", "VISOR DE ESCENAS", "ムービービューアー", "VISUALIZADOR DE ANIMAÇÕES",
      "PRZEGLĄDARKA SEKWENCJI FILMOWYCH", "ПРОСМОТР РОЛИКОВ", "TUSSENFILMS BEKIJKEN", "시네마틱 뷰어", "影片檢視器",
      "影片查看器", "عارض المشاهد السينمائية"}},
    // ModelViewer
    {-1, nullptr,
     {"MODEL VIEWER", "VISIONNEUSE DE MODÈLES", "MODELL-BETRACHTER", "VISUALIZZATORE MODELLI", "VISOR DE MODELOS",
      "VISOR DE MODELOS", "モデルビューアー", "VISUALIZADOR DE MODELOS", "PRZEGLĄDARKA MODELI", "ПРОСМОТР МОДЕЛЕЙ",
      "MODELLEN BEKIJKEN", "모델 뷰어", "模型檢視器", "模型查看器", "عارض النماذج"}},
    // Credits
    {2561, "CREDITS",
     {"CREDITS", "CRÉDITS", "MITWIRKENDE", "RICONOSCIMENTI", "CRÉDITOS", "CRÉDITOS", "クレジット", "CRÉDITOS",
      "TWÓRCY", "ТИТРЫ", "CREDITS", "제작진", "製作群", "制作团队", "الأسماء"}},
    // AboutCutsceneViewer
    {-1, nullptr,
     {"Watch the game's cutscenes again, in the story's outfit or any other.",
      "Revoyez les cinématiques du jeu, dans la tenue de l'histoire ou dans n'importe quelle autre.",
      "Sehen Sie sich die Filmsequenzen des Spiels noch einmal an, im Outfit der Story oder in einem anderen.",
      "Rivedi i filmati del gioco, con il completo della storia o con qualsiasi altro.",
      "Vuelve a ver las cinemáticas del juego con el atuendo de la historia o con cualquier otro.",
      "Vuelve a ver las escenas del juego con el atuendo de la historia o con cualquier otro.",
      "ゲームのムービーを、ストーリーのコスチュームや好きなコスチュームでもう一度観られます。",
      "Assista de novo às animações do jogo, com a roupa da história ou qualquer outra.",
      "Obejrzyj ponownie sekwencje filmowe z gry w stroju z głównego wątku lub w dowolnym innym.",
      "Пересмотрите ролики игры в сюжетном костюме или в любом другом.",
      "Bekijk de tussenfilms van het spel opnieuw, in de outfit van het verhaal of in een andere.",
      "스토리 복장이나 원하는 복장으로 게임의 시네마틱을 다시 감상합니다.",
      "以故事裝束或任何其他裝束，重溫遊戲的影片。",
      "以故事装束或任何其他装束，重温游戏的影片。",
      "شاهد المشاهد السينمائية للعبة مرة أخرى بزي القصة أو بأي زي آخر."}},
    // AboutModelViewer
    {-1, nullptr,
     {"Look at the game's characters and objects up close.",
      "Observez de près les personnages et les objets du jeu.",
      "Betrachten Sie die Figuren und Objekte des Spiels aus der Nähe.",
      "Osserva da vicino i personaggi e gli oggetti del gioco.",
      "Observa de cerca los personajes y objetos del juego.",
      "Observa de cerca los personajes y objetos del juego.",
      "ゲームのキャラクターやオブジェクトを間近で見られます。",
      "Veja de perto os personagens e objetos do jogo.",
      "Obejrzyj z bliska postacie i obiekty z gry.",
      "Рассмотрите персонажей и предметы игры вблизи.",
      "Bekijk de personages en objecten van het spel van dichtbij.",
      "게임의 캐릭터와 오브젝트를 가까이에서 살펴봅니다.",
      "近距離觀看遊戲中的角色與物件。",
      "近距离观看游戏中的角色与物品。",
      "شاهد شخصيات اللعبة وأغراضها عن قرب."}},
    // AboutCredits
    {-1, nullptr,
     {"The people who made Rise of the Tomb Raider.",
      "Les personnes qui ont créé Rise of the Tomb Raider.",
      "Die Menschen, die Rise of the Tomb Raider gemacht haben.",
      "Le persone che hanno realizzato Rise of the Tomb Raider.",
      "Las personas que crearon Rise of the Tomb Raider.",
      "Las personas que crearon Rise of the Tomb Raider.",
      "Rise of the Tomb Raider を作った人々。",
      "As pessoas que criaram Rise of the Tomb Raider.",
      "Ludzie, którzy stworzyli Rise of the Tomb Raider.",
      "Люди, создавшие Rise of the Tomb Raider.",
      "De mensen die Rise of the Tomb Raider hebben gemaakt.",
      "Rise of the Tomb Raider를 만든 사람들.",
      "打造 Rise of the Tomb Raider 的人們。",
      "打造 Rise of the Tomb Raider 的人们。",
      "الأشخاص الذين صنعوا Rise of the Tomb Raider."}},
    // Cutscene
    {-1, nullptr,
     {"CUTSCENE", "CINÉMATIQUE", "FILMSEQUENZ", "FILMATO", "CINEMÁTICA", "ESCENA", "ムービー", "ANIMAÇÃO",
      "SEKWENCJA FILMOWA", "РОЛИК", "TUSSENFILM", "시네마틱", "影片", "影片", "المشهد"}},
    // Outfit
    {-1, nullptr,
     {"OUTFIT", "TENUE", "OUTFIT", "COMPLETO", "ATUENDO", "ATUENDO", "コスチューム", "ROUPA", "STRÓJ", "КОСТЮМ",
      "OUTFIT", "복장", "裝束", "装束", "الزي"}},
    // Play
    {-1, nullptr,
     {"PLAY", "LECTURE", "ABSPIELEN", "RIPRODUCI", "REPRODUCIR", "REPRODUCIR", "再生", "REPRODUZIR", "ODTWÓRZ",
      "СМОТРЕТЬ", "AFSPELEN", "재생", "播放", "播放", "تشغيل"}},
    // StoryOutfit
    {-1, nullptr,
     {"Story outfit", "Tenue de l'histoire", "Outfit der Story", "Completo della storia", "Atuendo de la historia",
      "Atuendo de la historia", "ストーリーのコスチューム", "Roupa da história", "Strój z głównego wątku",
      "Сюжетный костюм", "Outfit van het verhaal", "스토리 복장", "故事裝束", "故事装束", "زي القصة"}},
    // NoneYet
    {-1, nullptr,
     {"None yet", "Aucune pour l'instant", "Noch keine", "Ancora nessuno", "Ninguna todavía", "Ninguna todavía",
      "まだありません", "Nenhuma ainda", "Jeszcze brak", "Пока нет", "Nog geen", "아직 없음", "尚無", "暂无",
      "لا يوجد بعد"}},
    // NoCutscenes
    {-1, nullptr,
     {"No cutscenes yet", "Aucune cinématique pour l'instant", "Noch keine Filmsequenzen", "Ancora nessun filmato",
      "Aún no hay cinemáticas", "Aún no hay escenas", "まだムービーがありません", "Ainda não há animações",
      "Brak sekwencji filmowych", "Пока нет роликов", "Nog geen tussenfilms", "아직 시네마틱이 없습니다", "尚無影片",
      "暂无影片", "لا توجد مشاهد بعد"}},
    // Outfits (the campsite's: 2924)
    {2924, "OUTFITS",
     {"OUTFITS", "TENUES", "OUTFITS", "COMPLETI", "ATUENDOS", "ATUENDOS", "コスチューム", "ROUPAS", "STROJE", "КОСТЮМЫ",
      "OUTFITS", "복장", "裝束", "装束", "الأزياء"}},
    // Characters
    {-1, nullptr,
     {"CHARACTERS", "PERSONNAGES", "FIGUREN", "PERSONAGGI", "PERSONAJES", "PERSONAJES", "キャラクター", "PERSONAGENS",
      "POSTACIE", "ПЕРСОНАЖИ", "PERSONAGES", "캐릭터", "角色", "角色", "الشخصيات"}},
    // Animals
    {-1, nullptr,
     {"ANIMALS", "ANIMAUX", "TIERE", "ANIMALI", "ANIMALES", "ANIMALES", "動物", "ANIMAIS", "ZWIERZĘTA", "ЖИВОТНЫЕ",
      "DIEREN", "동물", "動物", "动物", "الحيوانات"}},
    // Weapons (the campsite's: 3305)
    {3305, "WEAPONS",
     {"WEAPONS", "ARMES", "WAFFEN", "ARMI", "ARMAS", "ARMAS", "武器", "ARMAS", "BROŃ", "ОРУЖИЕ", "WAPENS", "무기", "武器",
      "武器", "الأسلحة"}},
    // Relics (the Collectibles': 2644)
    {2644, "RELICS",
     {"RELICS", "RELIQUES", "RELIKTE", "RELIQUIE", "RELIQUIAS", "RELIQUIAS", "レリック", "RELÍQUIAS", "RELIKTY", "РЕЛИКВИИ",
      "RELIEKEN", "유물", "遺物", "遗物", "الآثار"}},
    // AllUpgrades (v0.9.438)
    {-1, nullptr,
     {"ALL UPGRADES", "TOUTES LES AMÉLIORATIONS", "ALLE VERBESSERUNGEN", "TUTTI I POTENZIAMENTI", "TODAS LAS MEJORAS",
      "TODAS LAS MEJORAS", "全アップグレード", "TODAS AS MELHORIAS", "WSZYSTKIE ULEPSZENIA", "ВСЕ УЛУЧШЕНИЯ",
      "ALLE UPGRADES", "모든 업그레이드", "所有升級", "所有升级", "جميع التحسينات"}},
    // Search
    {-1, nullptr,
     {"SEARCH", "RECHERCHER", "SUCHEN", "CERCA", "BUSCAR", "BUSCAR", "検索", "PESQUISAR", "SZUKAJ", "ПОИСК", "ZOEKEN", "검색",
      "搜尋", "搜索", "بحث"}},
};

// localstr_get(int id, bool* found) (retail 0x832D0, TR2 0x95980; its first 5 bytes may be CustomOutfits' hook's
// jump, so the bytes after them are checked) and localstr_get_language() (retail 0x833A0, TR2 0x95A50: mov eax,
// [rip + x]; ret; localstr_get_language_code follows at +0x10: cmp ecx, 0x13).
constexpr std::uintptr_t kRvaLocalstrGet = 0x000832D0ull, kRvaLocalstrGetLanguage = 0x000833A0ull;
constexpr std::uint8_t kLocalstrGetTail[] = {0xC6, 0x02, 0x01, 0x83, 0xF9, 0xFF, 0x75, 0x10};
using LocalstrGetFn = const char*(__fastcall*)(int, bool*);

std::mutex g_mutex;
int g_codeState = 0;   // 0 not checked, 1 as expected, -1 not (logged; the texts are English then)
const int* g_language = nullptr;
int g_gameIdState[Count] = {};   // -1: the game's string is not the one expected (seen in English): ours
int g_lastLanguage = -2;
struct Cache {
    std::array<std::string, Count> text, upper;
    std::array<bool, Count> made{}, madeUpper{};
};
std::map<int, Cache> g_cache;   // by language (its strings are never changed once made: their pointers stay valid)

bool CodeMatches() {
    if (g_codeState) return g_codeState > 0;
    g_codeState = -1;
    const std::uintptr_t base = GameBuild::Base();
    const auto* get = reinterpret_cast<const std::uint8_t*>(base + kRvaLocalstrGet);
    const auto* language = reinterpret_cast<const std::uint8_t*>(base + kRvaLocalstrGetLanguage);
    const bool ok = std::memcmp(get + 5, kLocalstrGetTail, sizeof(kLocalstrGetTail)) == 0 && language[0] == 0x8B &&
                    language[1] == 0x05 && language[6] == 0xC3 && language[0x10] == 0x83 && language[0x11] == 0xF9 &&
                    language[0x12] == 0x13;
    if (!ok) {
        Log::Write("ViewerText: the game's localstr functions are not where expected; the Extras menus are in English");
        return false;
    }
    g_language = reinterpret_cast<const int*>(language + 6 + *reinterpret_cast<const std::int32_t*>(language + 2));
    g_codeState = 1;
    return true;
}

int Language() { return CodeMatches() ? *g_language : 0; }

// SEH only.
const char* GameString(int id, bool& found) noexcept {
    found = false;
    __try {
        return reinterpret_cast<LocalstrGetFn>(GameBuild::Base() + kRvaLocalstrGet)(id, &found);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const std::string& Make(Id id, int language, Cache& cache) {
    if (cache.made[id]) return cache.text[id];
    cache.made[id] = true;
    const Entry& entry = kEntries[id];
    const int column = Column(language);
    std::string text;
    if (entry.gameId >= 0 && g_gameIdState[id] >= 0 && CodeMatches()) {
        bool found = false;
        const char* game = GameString(entry.gameId, found);
        if (game && found && game[0]) {
            if (column == 0 && std::strcmp(game, entry.english) != 0) {
                g_gameIdState[id] = -1;
                Log::Writef("ViewerText: the game's string %d is \"%s\", not \"%s\": ours is used", entry.gameId, game,
                            entry.english);
            } else {
                text = game;
            }
        }
    }
    if (text.empty()) {
        const char* ours = entry.text[column] ? entry.text[column] : entry.text[0];
        text = column == kArabicColumn ? ArabicDisplay::FromLogical(ours) : std::string(ours);
    }
    cache.text[id] = std::move(text);
    return cache.text[id];
}
}  // namespace

std::string ToUpper(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (wide <= 0) return utf8;
    std::wstring w(std::size_t(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), wide);
    std::wstring upper(w.size() * 2 + 4, L'\0');   // (a letter can become two: ß -> SS)
    const int made = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), int(w.size()), upper.data(),
                                   int(upper.size()), nullptr, nullptr, 0);
    if (made <= 0) return utf8;
    upper.resize(std::size_t(made));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, upper.data(), made, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return utf8;
    std::string out(std::size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, upper.data(), made, out.data(), bytes, nullptr, nullptr);
    return out;
}

const char* Get(Id id) {
    if (id < 0 || id >= Count) return "";
    std::lock_guard lock(g_mutex);
    const int language = Language();
    if (language != g_lastLanguage) {
        g_lastLanguage = language;
        Log::Writef("ViewerText: the Extras menus in %s (the game's text language %d)",
                    language >= 0 && language < int(std::size(kLanguageNames)) ? kLanguageNames[language] : "English",
                    language);
    }
    return Make(id, language, g_cache[language]).c_str();
}

const char* Upper(Id id) {
    if (id < 0 || id >= Count) return "";
    const char* text = Get(id);
    std::lock_guard lock(g_mutex);
    Cache& cache = g_cache[Language()];
    if (!cache.madeUpper[id]) {
        cache.madeUpper[id] = true;
        cache.upper[id] = ToUpper(text);
    }
    return cache.upper[id].c_str();
}

const char* LanguageCode() {
    std::lock_guard lock(g_mutex);
    return kCodes[Column(Language())];
}

std::string Localized(const std::map<std::string, std::string>& texts, const std::string& fallback) {
    int column = 0;
    {
        std::lock_guard lock(g_mutex);
        column = Column(Language());
    }
    const auto it = texts.find(kCodes[column]);
    const std::string& text = it != texts.end() && !it->second.empty() ? it->second : fallback;
    return column == kArabicColumn ? ArabicDisplay::FromLogical(text) : text;
}

static_assert(kColumns == kLanguageCount, "Pick's texts are our columns");
std::string Pick(const char* const (&texts)[kLanguageCount]) {
    int column = 0;
    {
        std::lock_guard lock(g_mutex);
        column = Column(Language());
    }
    const char* text = texts[column] && texts[column][0] ? texts[column] : texts[0];
    if (!text) return {};
    return column == kArabicColumn ? ArabicDisplay::FromLogical(text) : std::string(text);
}

std::string GameText(int id) {
    if (id < 0) return {};
    {
        std::lock_guard lock(g_mutex);
        if (!CodeMatches()) return {};
    }
    bool found = false;
    const char* text = GameString(id, found);
    if (!text || !found) return {};
    std::string out;
    for (std::size_t k = 0; k < 4096; ++k) {   // (the game's own string: read as the game does)
        const char c = text[k];
        if (!c) break;
        out += c;
    }
    return out;
}

const char* LanguageName() {
    std::lock_guard lock(g_mutex);
    const int language = Language();
    return language >= 0 && language < int(std::size(kLanguageNames)) ? kLanguageNames[language] : "English";
}
}  // namespace ViewerText
