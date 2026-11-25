// (UTF-8: built with /utf-8 under MSVC)
#include "cutscene_titles.h"

#include <cctype>
#include <cstring>

namespace CutsceneTitles {
namespace {
// The game's language codes for our columns (localstr_get_language_code, retail 0x833B0: its table names en, fr, de,
// it, es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, en, ar, cs, es, hu, en for language_t 0 to 19).
constexpr int kTranslations = 14;
const char* const kCodes[kTranslations] = {"fr", "de", "it", "es-mx", "es", "ja", "pt", "pl", "ru", "nl", "ko", "cht",
                                           "chs", "ar"};

// The titles of the cutscenes the viewer lists (the ones with a start in the game's Chapter Select, and A Choice that
// comes with Revealing the Path). In the game's own words where it has them (locals.bin, its 16 copies): the story
// missions To the Tower (986) and Alone Again (1152), the Remnant Resistance mode (2343), the Remnant (1110: Rescapés,
// Verbliebenen, Discendenti, Remanente / Resto, 住民, Remanescentes, Ocalali, Потомки, volgelingen, 원주민, 遺民 / 遗民,
// البقايا), the Divine Source (1215), Sofia (1111: Софья), Ana (1243: Анна), Konstantin (1245: Константин,
// كونسطانطين), the Cathedral (1179), the shotgun (3252), Revelations (4576: a journal's title), Execution (5357; not
// its Japanese 実行, a program's "run"), Aftermath (4457), the Atlas (7108) and the Deathless Ones (4905; without the
// period of its Russian and Dutch). The rest are ours.
struct Row {
    const char* id;
    const char* english;
    const char* text[kTranslations];
};
const Row kRows[] = {
    {"01_rockbottom_cine", "Rock Bottom",
     {"Au plus bas", "Am Tiefpunkt", "Toccare il fondo", "Tocar fondo", "Tocar fondo", "どん底", "No fundo do poço",
      "Na dnie", "На самом дне", "Dieptepunt", "밑바닥", "跌落谷底", "跌落谷底", "في الحضيض"}},
    {"03_carcrash_cine_03_part01", "Car Crash",
     {"Accident de voiture", "Autounfall", "Incidente d'auto", "Choque de auto", "Accidente de coche", "自動車事故",
      "Acidente de carro", "Wypadek samochodowy", "Автокатастрофа", "Auto-ongeluk", "자동차 사고", "車禍", "车祸",
      "حادث السيارة"}},
    {"04_thisistheplace_cine", "This Is the Place",
     {"C'est ici", "Hier ist es", "È qui", "Es aquí", "Es aquí", "ここだ", "É aqui", "To tutaj", "Это здесь",
      "Hier is het", "바로 여기", "就是這裡", "就是这里", "هذا هو المكان"}},
    {"08_emptytomb_cine", "Empty Tomb",
     {"Le tombeau vide", "Das leere Grab", "La tomba vuota", "La tumba vacía", "La tumba vacía", "空の墓",
      "O túmulo vazio", "Pusty grobowiec", "Пустая гробница", "De lege tombe", "빈 무덤", "空墓", "空墓", "القبر الفارغ"}},
    {"10_revelations_cine", "Revelations",
     {"Révélations", "Enthüllungen", "Rivelazioni", "Revelaciones", "Revelaciones", "新事実", "Revelações",
      "Rewelacje", "Откровения", "Onthullingen", "드러난 진실", "真相浮現", "真相浮现", "اكتشافات"}},
    {"14_expedtion_a_cine", "Long Journey",
     {"Un long voyage", "Eine lange Reise", "Un lungo viaggio", "Un largo viaje", "Un largo viaje", "長い旅",
      "Uma longa jornada", "Długa podróż", "Долгий путь", "Een lange reis", "머나먼 여정", "漫長旅程", "漫长旅程",
      "رحلة طويلة"}},
    {"15_almostthere_cine", "Almost There",
     {"Presque arrivés", "Fast geschafft", "Quasi arrivati", "Ya casi", "Ya casi", "あと少し", "Quase lá", "Już prawie",
      "Почти на месте", "Bijna daar", "거의 다 왔다", "就快到了", "就快到了", "اقتربنا"}},
    {"20_alive_cine", "Alive",
     {"En vie", "Am Leben", "Viva", "Viva", "Viva", "生還", "Viva", "Żywa", "Жива", "In leven", "생존", "倖存", "幸存",
      "على قيد الحياة"}},
    {"21_whispersinthedark_cine", "Whispers in the Dark",
     {"Murmures dans le noir", "Flüstern im Dunkeln", "Sussurri nel buio", "Susurros en la oscuridad",
      "Susurros en la oscuridad", "闇の囁き", "Sussurros no escuro", "Szepty w ciemności", "Шёпот во тьме",
      "Gefluister in het donker", "어둠 속의 속삭임", "黑暗中的低語", "黑暗中的低语", "همسات في الظلام"}},
    {"22a_theenemy_cine", "The Enemy",
     {"L'ennemi", "Der Feind", "Il nemico", "El enemigo", "El enemigo", "敵", "O inimigo", "Wróg", "Враг", "De vijand",
      "적", "敵人", "敌人", "العدو"}},
    {"22b_sofia_cine", "Sofia",
     {"Sofia", "Sofia", "Sofia", "Sofia", "Sofia", "ソフィア", "Sofia", "Sofia", "Софья", "Sofia", "소피아", "索菲亞",
      "索菲亚", "صوفيا"}},
    {"23_aconnection_cine", "A Connection",
     {"Un lien", "Eine Verbindung", "Un collegamento", "Una conexión", "Una conexión", "つながり", "Uma conexão",
      "Połączenie", "Связь", "Een verbinding", "연결", "聯繫", "联系", "اتصال"}},
    {"26_ana_cine", "Ana",
     {"Ana", "Ana", "Ana", "Ana", "Ana", "アナ", "Ana", "Ana", "Анна", "Ana", "아나", "安娜", "安娜", "آنا"}},
    {"27b_trustissues_cine", "Trust Issues",
     {"Question de confiance", "Vertrauensfrage", "Questione di fiducia", "Problemas de confianza",
      "Cuestión de confianza", "信頼の問題", "Questão de confiança", "Kwestia zaufania", "Вопрос доверия",
      "Een kwestie van vertrouwen", "신뢰의 문제", "信任問題", "信任问题", "مسألة ثقة"}},
    {"28_atlas_cine", "The Atlas",   // (the game's, 7108)
     {"L'Atlas", "Der Atlas", "L'Atlante", "El Atlas", "El Atlas", "アトラス", "O Atlas", "Atlas", "Атлас", "De Atlas",
      "아틀라스", "亞特拉斯輿圖", "阿特拉斯地图", "الأطلس"}},
    {"30_thedrowning_cine_b", "The Drowning",
     {"La noyade", "Das Ertrinken", "L'annegamento", "El ahogamiento", "El ahogamiento", "溺水", "O afogamento",
      "Tonięcie", "Утопление", "De verdrinking", "익사", "溺水", "溺水", "الغرق"}},
    {"31a_aloneagain_cine", "Alone Again",   // (the game's mission, 1152)
     {"En solitaire", "Wieder allein", "Di nuovo in solitaria", "Sola otra vez", "Sola de nuevo", "また独り",
      "Sozinha de novo", "Znów sama", "Снова одна", "Wederom alleen", "또다시 혼자", "再次孤獨", "再次孤独",
      "وحدي مجددا"}},
    {"35_inthroughtheoutdoor_cine", "In Through the Out Door",
     {"Entrer par la sortie", "Rein durch den Ausgang", "Entrare dall'uscita", "Entrar por la salida",
      "Entrar por la salida", "出口から入る", "Entrar pela saída", "Wejście przez wyjście", "Вход через выход",
      "Binnen via de uitgang", "출구로 진입", "從出口進入", "从出口进入", "الدخول من المخرج"}},
    {"36_meettheremnant_cine", "Meet the Remnant",
     {"Rencontre avec les Rescapés", "Begegnung mit den Verbliebenen", "Incontro con i Discendenti",
      "Encuentro con el Remanente", "Encuentro con el Resto", "住民との出会い", "Encontro com os Remanescentes",
      "Spotkanie z Ocalałymi", "Встреча с Потомками", "Ontmoeting met de volgelingen", "원주민과의 만남", "與遺民相遇",
      "与遗民相遇", "لقاء البقايا"}},
    {"39_shotgun_cine", "Shotgun",   // (the game's weapon, 3252)
     {"Fusil à pompe", "Schrotflinte", "Fucile pesante", "Escopeta", "Escopeta", "ショットガン", "Escopeta", "Strzelba",
      "Дробовик", "Jachtgeweer", "샷건", "霰彈槍", "霰弹枪", "بندقية خرطوش"}},
    {"39b_setback_cine", "Setback",
     {"Revers", "Rückschlag", "Battuta d'arresto", "Revés", "Revés", "挫折", "Revés", "Niepowodzenie", "Неудача",
      "Tegenslag", "좌절", "挫敗", "挫败", "انتكاسة"}},
    {"40_tothetower_cine", "To the Tower",   // (the game's mission, 986)
     {"À la tour", "Auf zum Turm", "Alla torre", "Hacia la torre", "A la torre", "塔へ", "Para a torre", "Do wieży",
      "К башне", "Naar de toren", "탑으로", "前往塔樓", "前往塔楼", "إلى البرج"}},
    {"40b_courtyardincursion_cine", "Courtyard Incursion",
     {"Incursion dans la cour", "Vorstoß in den Hof", "Incursione nel cortile", "Incursión en el patio",
      "Incursión en el patio", "中庭への突入", "Incursão no pátio", "Wtargnięcie na dziedziniec", "Вторжение во двор",
      "Inval op de binnenplaats", "안뜰 습격", "突襲中庭", "突袭中庭", "اقتحام الفناء"}},
    {"40c_remnantresistance_cine", "Remnant Resistance",   // (the game's mode, 2343)
     {"La résistance des Rescapés", "Widerstand der Verbliebenen", "Resistenza dei Discendenti",
      "Resistencia del Remanente", "Resistencia del Resto", "住民の抵抗", "Resistência Remanescente", "Opór Ocalałych",
      "Сопротивление Потомков", "Verzet van de volgelingen", "원주민의 저항", "遺民反抗行動", "遗民反抗行动",
      "مقاومة البقايا"}},
    {"40d_planofaction_cine", "Plan of Action",
     {"Plan d'action", "Schlachtplan", "Piano d'azione", "Plan de acción", "Plan de acción", "作戦会議", "Plano de ação",
      "Plan działania", "План действий", "Plan van aanpak", "작전 계획", "行動計畫", "行动计划", "خطة العمل"}},
    {"41_retreat_cine", "Retreat",
     {"Retraite", "Rückzug", "Ritirata", "Retirada", "Retirada", "撤退", "Retirada", "Odwrót", "Отступление",
      "Terugtocht", "후퇴", "撤退", "撤退", "الانسحاب"}},
    {"44_cathedralambush_cine", "Cathedral Ambush",
     {"Embuscade à la cathédrale", "Hinterhalt in der Kathedrale", "Imboscata nella cattedrale",
      "Emboscada en la catedral", "Emboscada en la catedral", "大聖堂の待ち伏せ", "Emboscada na catedral",
      "Zasadzka w katedrze", "Засада в Соборе", "Hinderlaag in de kathedraal", "대성당 매복", "大教堂伏擊", "大教堂伏击",
      "كمين الكاتدرائية"}},
    {"46_thedeathlessones_cine", "The Deathless Ones",   // (the game's, 4905)
     {"Les Immortels", "Die Unsterblichen", "Gli Immortali", "Los inmortales", "Los inmortales", "不滅隊", "Os Imortais",
      "Nieśmiertelni", "Бессмертные", "De Onsterfelijken", "불멸의 존재들", "不死軍團", "不死军团", "الحرس المنيع"}},
    {"47b_reunion_cine", "Reunion",
     {"Retrouvailles", "Wiedersehen", "Ricongiungimento", "Reencuentro", "Reencuentro", "再会", "Reencontro",
      "Ponowne spotkanie", "Воссоединение", "Hereniging", "재회", "重逢", "重逢", "لم الشمل"}},
    {"47c_revealingthepath_cine", "Revealing the Path",
     {"Le chemin révélé", "Der Weg offenbart sich", "Il sentiero svelato", "El camino revelado", "El camino revelado",
      "示された道", "O caminho revelado", "Odkrycie ścieżki", "Путь открывается", "Het pad onthuld", "드러난 길",
      "道路揭曉", "道路揭晓", "كشف الطريق"}},
    {"47d_achoice_cine", "A Choice",
     {"Un choix", "Eine Entscheidung", "Una scelta", "Una elección", "Una elección", "選択", "Uma escolha", "Wybór",
      "Выбор", "Een keuze", "선택", "抉擇", "抉择", "خيار"}},
    {"48a_locatingthesource_cine", "Locating the Source",
     {"Localiser la Source", "Die Quelle finden", "Localizzare la Sorgente", "Localizar la Fuente",
      "Localizar la Fuente", "源の在り処", "Localizando a Fonte", "Namierzanie Źródła", "Поиски Источника",
      "De Bron vinden", "원천을 찾아서", "尋找神聖之源", "寻找神圣之源", "تحديد موقع المصدر"}},
    {"50_anoldfriend_cine", "An Old Friend",
     {"Un vieil ami", "Ein alter Freund", "Un vecchio amico", "Un viejo amigo", "Un viejo amigo", "旧友",
      "Um velho amigo", "Stary przyjaciel", "Старый друг", "Een oude vriend", "오랜 친구", "老朋友", "老朋友",
      "صديق قديم"}},
    {"51_tehrace_cine", "The Race",
     {"La course", "Der Wettlauf", "La corsa", "La carrera", "La carrera", "競争", "A corrida", "Wyścig", "Гонка",
      "De race", "경주", "競賽", "竞赛", "السباق"}},
    {"56_theconstanceofkonstantin_cine", "The Constance of Konstantin",
     {"La constance de Konstantin", "Konstantins Beständigkeit", "La costanza di Konstantin",
      "La constancia de Konstantin", "La constancia de Konstantin", "コンスタンティンの信念",
      "A constância de Konstantin", "Niezłomność Konstantina", "Постоянство Константина",
      "De standvastigheid van Konstantin", "콘스탄틴의 신념", "康斯坦汀的信念", "康斯坦汀的信念", "ثبات كونسطانطين"}},
    {"57_thereckoning_cine", "The Reckoning",
     {"Le règlement de comptes", "Die Abrechnung", "La resa dei conti", "El ajuste de cuentas", "El ajuste de cuentas",
      "決着", "O acerto de contas", "Rozrachunek", "Расплата", "De afrekening", "결판", "清算", "清算", "تصفية الحساب"}},
    {"57b_execution_cine", "Execution",
     {"Exécution", "Hinrichtung", "Esecuzione", "Ejecución", "Ejecución", "処刑", "Execução", "Egzekucja", "Казнь",
      "Executie", "처형", "處決", "处决", "إعدام"}},
    {"59_anewbeginning_cine", "A New Beginning",
     {"Un nouveau départ", "Ein Neuanfang", "Un nuovo inizio", "Un nuevo comienzo", "Un nuevo comienzo",
      "新たな始まり", "Um novo começo", "Nowy początek", "Новое начало", "Een nieuw begin", "새로운 시작", "新的開始",
      "新的开始", "بداية جديدة"}},
    {"60_aftermath_cine", "Aftermath",
     {"Les répercussions", "Nachwirkungen", "Conseguenze", "Consecuencias", "Consecuencias", "その後",
      "Consequências", "Pokłosie", "Последствия", "Nasleep", "여파", "餘波", "余波", "العواقب"}},
};

bool SameText(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
}  // namespace

const std::map<std::string, std::string>* Translations(const std::string& id, const std::string& englishTitle) {
    static const std::map<std::string, std::map<std::string, std::string>> byId = [] {
        std::map<std::string, std::map<std::string, std::string>> m;
        for (const Row& row : kRows) {
            auto& texts = m[row.id];
            for (int i = 0; i < kTranslations; ++i)
                if (row.text[i] && row.text[i][0]) texts[kCodes[i]] = row.text[i];
        }
        return m;
    }();
    std::string key = id;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const Row& row : kRows)
        if (key == row.id) {
            if (!SameText(englishTitle, row.english)) return nullptr;
            const auto it = byId.find(row.id);
            return it != byId.end() ? &it->second : nullptr;
        }
    return nullptr;
}

}  // namespace CutsceneTitles
