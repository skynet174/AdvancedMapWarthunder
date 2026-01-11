#include "Translator.h"
#include <Windows.h>
#include <vector>

namespace {
    // helper to convert wide -> utf8
    static std::string WideToUtf8(const wchar_t* wstr) {
        if (!wstr) return {};
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) return {};
        std::vector<char> buf(size_needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf.data(), size_needed, nullptr, nullptr);
        return std::string(buf.data());
    }
}

Translator::Translator() {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {0};
    std::string langCode = "fr"; // fallback initial

    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH)) {
        std::string full = WideToUtf8(localeName);
        if (!full.empty()) {
            if (full.size() >= 2) {
                langCode = full.substr(0, 2);
                for (auto& c : langCode) c = (char)tolower(c);
            }
        }
    }

    translations["fr"] = {
        {"indicators_header", "Indicateurs :"},
        {"indicators_no_data", "Indicateurs : Pas de données"},
        {"speed_data_fmt", "Vitesse : %.1f"},
        {"fuel_data_fmt", "Carburant : %.0f"},
        {"tank_data_fmt", "Données du char : %s"},
        {"throttle_data_fmt", "Manette des gaz : %.0f%%"},
        {"aircraft_data_fmt", "Données de l'avion : %s"},
        {"compass_data_fmt", "Cap : %.1f°"},
        {"mach_data_fmt", "Nombre de Mach : %.2f"},
        {"g_meter_data_fmt", "Facteur de charge : %.2f"},
        {"gear_released_state", " (déployé)"},
        {"gear_retracted_state", " (rentré)"},
        {"gear_in_progress_state", " (en cours)"},
        {"tank_data", "Données du char"},
        {"aircraft_data", "Données de l'avion"},
        {"state_header", "État :"},
        {"state_no_data", "État : Pas de données"},
        {"altitude_fmt", "Altitude : %d m"},
        {"altitude_fmt_m", "Altitude : %.1f m"},
        {"tas_fmt", "Vitesse (TAS) : %d km/h"},
        {"ias_fmt", "Vitesse (IAS) : %d km/h"},
        {"mach_fmt", "Mach : %.2f"},
        {"aoa_fmt", "Angle d'attaque : %.1f°"},
        {"vy_fmt", "Vitesse verticale : %.1f m/s"},
        {"fuel_fmt", "Carburant : %d / %d kg"},
        {"fuel_percent_fmt", "Carburant : %.1f%%"},
        {"gears_status_fmt", "Trains : %.0f%%%s"},
        {"flaps_fmt", "Volets : %.0f%%"},
        {"throttle1_fmt", "Manette des gaz 1 : %d%%"},
        {"rpm1_fmt", "Régime 1 : %d"},
        {"power1_fmt", "Puissance 1 : %.1f ch"},
        {"mission_header", "Mission :"},
        {"mission_no_data", "Mission : Pas de données"},
        {"status_label", "Statut : "},
        {"status_running", "En cours"},
        {"status_fail", "Échouée"},
        {"objectives_label", "Objectifs :"},
        {"objective_primary", "  [Primaire]"},
        {"objective_secondary", "  [Secondaire]"},
        {"objective_status_label", "  Statut : "},
        {"status_in_progress", "En cours"},
        {"status_completed", "Terminé"},
        {"status_failed", "Échoué"},
        {"objective_text_fmt", "  Objectif : %s"},
        {"no_objectives", "Aucun objectif"},
        {"hide_chat", "Cacher le chat"},
        {"show_chat", "Afficher le chat"},
        {"debug_on", "Debug : ON"},
        {"debug_off", "Debug : OFF"},
        {"follow_on", "Suivi : ON (F)"},
        {"follow_off", "Suivi : OFF (F)"},
        {"follow_tooltip", "Suivi (F)\nMolette - ajuster le zoom\nGlisser ou clic milieu pour désactiver"},
        {"clear_tooltip", "Effacer toutes les sélections et les marqueurs (C)"},
        {"chat_tab", "Chat"},
        {"events_tab", "Combats"},
        {"no_events", "Aucun événement"},
        {"marker_tooltip_fmt", "Marqueur #%d\n[Clic droit pour supprimer]"},
        {"player_disconnected_fmt", "%s s'est déconnecté du jeu"},
        {"player_disconnected_no_name", "Un joueur s'est déconnecté du jeu"},
        {"player_lost_connection_fmt", "%s a perdu la connexion"},
        {"player_lost_connection_no_name", "Un joueur a perdu la connexion"},
        {"unit_label_fmt", "Unité : %s"},
        {"type_label_fmt", "Type : %s"},
        {"grid_label_fmt", "Case : %s"},
        {"position_fmt", "Position en jeu : %.1f, %.1f"},
        {"distance_to_player_fmt", "Distance au joueur : %.1f m"},
        {"cursor_grid_fmt", "Case : %s"},
        {"cursor_game_coord_fmt", "Coordonnée sous le curseur (jeu) : %.1f, %.1f"},
        {"cursor_pixel_coord_fmt", "Coordonnée sous le curseur (pixels) : %.0f, %.0f"}
    };

    translations["ru"] = {
        {"indicators_header", "Индикаторы:"},
        {"indicators_no_data", "Индикаторы: Нет данных"},
        {"speed_data_fmt", "Скорость: %.1f"},
        {"fuel_data_fmt", "Топливо: %.0f"},
        {"throttle_data_fmt", "Рычаг газа: %.0f%%"},
        {"tank_data_fmt", "Данные танка: %s"},
        {"aircraft_data_fmt", "Данные самолета: %s"},
        {"compass_data_fmt", "Компас: %.1f°"},
        {"mach_data_fmt", "Число Маха: %.2f"},
        {"g_meter_data_fmt", "Перегрузка: %.2f"},
        {"gear_released_state", " (выпущено)"},
        {"gear_retracted_state", " (убрано)"},
        {"gear_in_progress_state", " (в процессе)"},
        {"tank_data", "Данные танка"},
        {"aircraft_data", "Данные самолета"},
        {"state_header", "Состояние:"},
        {"state_no_data", "Состояние: Нет данных"},
        {"altitude_fmt", "Высота: %d м"},
        {"altitude_fmt_m", "Высота: %.1f м"},
        {"tas_fmt", "Скорость (истинная): %d км/ч"},
        {"ias_fmt", "Скорость (приборная): %d км/ч"},
        {"mach_fmt", "Число Маха: %.2f"},
        {"aoa_fmt", "Угол атаки: %.1f°"},
        {"vy_fmt", "Вертикальная скорость: %.1f м/с"},
        {"fuel_fmt", "Топливо: %d / %d кг"},
        {"fuel_percent_fmt", "Топливо: %.1f%%"},
        {"gears_status_fmt", "Шасси: %.0f%%%s"},
        {"flaps_fmt", "Закрылки: %.0f%%"},
        {"throttle1_fmt", "Рычаг газа 1: %d%%"},
        {"rpm1_fmt", "Обороты 1: %d"},
        {"power1_fmt", "Мощность 1: %.1f л.с."},
        {"mission_header", "Миссия:"},
        {"mission_no_data", "Миссия: Нет данных"},
        {"status_label", "Статус: "},
        {"status_running", "В процессе"},
        {"status_fail", "Провалена"},
        {"objectives_label", "Цели:"},
        {"objective_primary", "  [Основная]"},
        {"objective_secondary", "  [Вторичная]"},
        {"objective_status_label", "  Статус: "},
        {"status_in_progress", "В процессе"},
        {"status_completed", "Выполнено"},
        {"status_failed", "Провалено"},
        {"objective_text_fmt", "  Задача: %s"},
        {"no_objectives", "Нет целей"},
        {"hide_chat", "Скрыть чат"},
        {"show_chat", "Показать чат"},
        {"debug_on", "Debug: ВКЛ"},
        {"debug_off", "Debug: ВЫКЛ"},
        {"follow_on", "Слежение: ВКЛ (F)"},
        {"follow_off", "Слежение: ВЫКЛ (F)"},
        {"follow_tooltip", "Слежение (F)\nКолесо мыши - корректировка зума\nПеретаскивание или СКМ для отключения"},
        {"clear_tooltip", "Очистить все выделения и метки (C)"},
        {"chat_tab", "Чат"},
        {"events_tab", "Сражения"},
        {"no_events", "Нет событий"},
        {"marker_tooltip_fmt", "Метка #%d\n[ПКМ для удаления]"},
        {"player_disconnected_fmt", "%s отключился от игры"},
        {"player_disconnected_no_name", "Игрок отключился от игры"},
        {"player_lost_connection_fmt", "%s потерял связь"},
        {"player_lost_connection_no_name", "Игрок потерял связь"},
        {"unit_label_fmt", "Юнит: %s"},
        {"type_label_fmt", "Тип: %s"},
        {"grid_label_fmt", "Квадрат: %s"},
        {"position_fmt", "Позиция в игре: %.1f, %.1f"},
        {"distance_to_player_fmt", "Расстояние до игрока: %.1f м"},
        {"cursor_grid_fmt", "Квадрат: %s"},
        {"cursor_game_coord_fmt", "Координата под курсором игровая: %.1f, %.1f"},
        {"cursor_pixel_coord_fmt", "Координата под курсором в пикселях: %.0f, %.0f"}
    };

    //langCode = "ru";
    currentLang = langCode;
}

const std::string& Translator::Get(const std::string& key) const {
    // try current language
    auto itLang = translations.find(currentLang);
    if (itLang != translations.end()) {
        auto it = itLang->second.find(key);
        if (it != itLang->second.end()) return it->second;
    }
    // fallback to Russian
    auto itRu = translations.find("ru");
    if (itRu != translations.end()) {
        auto it2 = itRu->second.find(key);
        if (it2 != itRu->second.end()) return it2->second;
    }
    // final fallback: return key itself
    return key;
}

void Translator::SetLang(const std::string& lang) {
    if (!lang.empty()) {
        currentLang = lang;
    }
}

Translator& TR() {
    static Translator instance;
    return instance;
}