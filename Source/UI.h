#pragma once

#include "imgui.h"
#include <string>
#include <vector>
#include <mutex>

// Структура для сообщения чата
struct ChatMessage {
    int id;
    std::string msg;
    std::string sender;
    bool enemy;
    std::string mode;
};

// Структура для события (hudmsg)
struct EventMessage {
    int id;
    std::string msg;
    std::string sender;
    bool enemy;
    std::string mode;
};

// Структура для данных indicators
struct IndicatorsData {
    bool valid = false;
    std::string type;
    float speed = 0.0f;
    float altitude_hour = 0.0f;
    float altitude_min = 0.0f;
    float compass = 0.0f;
    float mach = 0.0f;
    float g_meter = 0.0f;
    float fuel = 0.0f;
    float throttle = 0.0f;
    float gears = 0.0f;
    float flaps = 0.0f;
};

// Структура для данных state
struct StateData {
    bool valid = false;
    int altitude = 0;       // H, m
    int tas = 0;            // TAS, km/h
    int ias = 0;            // IAS, km/h
    float mach = 0.0f;      // M
    float aoa = 0.0f;       // AoA, deg
    float vy = 0.0f;        // Vy, m/s
    int fuel = 0;           // Mfuel, kg
    int fuel0 = 0;          // Mfuel0, kg
    int throttle1 = 0;      // throttle 1, %
    int rpm1 = 0;           // RPM 1
    float power1 = 0.0f;    // power 1, hp
};

// Структура для цели миссии
struct MissionObjective {
    bool primary = false;
    std::string status; // "in_progress", "completed", "failed"
    std::string text;
};

// Структура для данных mission
struct MissionData {
    bool valid = false;
    std::string status; // "running", "fail"
    std::vector<MissionObjective> objectives;
};

// Структура для данных map_info
struct MapInfoData {
    bool valid = false;
    ImVec2 gridSteps = ImVec2(5000.0f, 5000.0f);
    ImVec2 gridZero;
    ImVec2 mapMin = ImVec2(-65536.0f, -65536.0f);
    ImVec2 mapMax = ImVec2(65536.0f, 65536.0f);
    int hudType = 0;
    int mapGeneration = 0;
};

// Структура для объектов карты (map_obj.json)
struct MapObject {
    std::string type;
    std::string icon;
    ImVec2 pos;                         // Нормализованные координаты (0-1)
    ImVec2 dir;                         // Направление (для aircraft)
    ImVec2 lineStart;                   // Для линий (аэродром) - начало
    ImVec2 lineEnd;                     // Для линий (аэродром) - конец
    float r = 1.0f, g = 1.0f, b = 1.0f; // Цвет (нормализованный 0-1)
    std::string colorHash;              // Хеш цвета для идентификации (для aircraft)
    bool isPlayer = false;
    bool initialized = false;

    // Для отслеживания движения и предсказания позиции
    ImVec2 lastPos;                     // Последние координаты
    ImVec2 lastDir;                     // Последнее направление
    float lastUpdateTime = 0.0f;        // Время последнего обновления
    int missedUpdates = 0;              // Количество пропущенных обновлений
};

// Структура для меток на карте
struct MapMarker {
    ImVec2 pos;  // Нормализованные координаты (0-1)
    unsigned char r = 255, g = 200, b = 0;  // Жёлтый по умолчанию
};

// Глобальные данные для UI (доступны из main.cpp)
extern std::vector<ChatMessage> g_chatMessages;
extern std::vector<EventMessage> g_eventMessages;
extern std::mutex g_chatMutex;
extern std::mutex g_eventMutex;
extern int g_lastChatId;
extern int g_lastEventId;

// Глобальные данные для indicators, state, mission и map_info
extern IndicatorsData g_indicatorsData;
extern StateData g_stateData;
extern MissionData g_missionData;
extern MapInfoData g_mapInfoData;
extern std::vector<MapObject> g_mapObjects;
extern std::vector<MapMarker> g_mapMarkers;
extern std::vector<size_t> g_selectedUnits;
// Структура для сохранения выбранных юнитов по координатам (для восстановления после пропажи)
struct SelectedUnitInfo {
    ImVec2 pos;             // Последние известные координаты
    std::string type, icon; // Тип и иконка для идентификации
    std::string colorHash;  // Хеш цвета для идентификации
    float lastSeenTime;     // Время последнего обновления
};
extern std::vector<SelectedUnitInfo> g_selectedUnitsBackup;
extern std::mutex g_indicatorsMutex;
extern std::mutex g_stateMutex;
extern std::mutex g_missionMutex;
extern std::mutex g_mapInfoMutex;
extern std::mutex g_mapObjectsMutex;
extern std::mutex g_mapMarkersMutex;

// Функции парсинга (вызываются из ApiFetcher callback'ов)
void ParseGameChat(const std::string& jsonData);
void ParseHudMsg(const std::string& jsonData);
void ParseIndicators(const std::string& jsonData);
void ParseState(const std::string& jsonData);
void ParseMission(const std::string& jsonData);
void ParseMapInfo(const std::string& jsonData);
void ParseMapObjects(const std::string& jsonData);

// Инициализация UI
void InitializeUI();

// Отрисовка UI
void RenderUI();

// Очистка UI
void ShutdownUI();

// Сохранение/загрузка настроек
void SaveSettings();
void LoadSettings();

