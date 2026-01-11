// Advanced Map War Thunder - Main Application
// ImGui интерфейс с асинхронным JSON парсером

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "UI.h"
#include "JsonParser.h"
#include "ApiFetcher.h"
#include "_FontEmbedded.h"
#include "dictionary.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <windows.h>
#include <dwmapi.h>

// DirectX 11 данные
ID3D11Device*                  g_pd3dDevice = nullptr; // Экспортируем для UI.cpp
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

HWND                           g_hWnd = nullptr; // Глобальный handle окна (используется в UI.cpp)

// JSON парсер (работает на отдельном ядре)
static JsonParser*              g_jsonParser = nullptr;

// API загрузчик (работает на отдельном потоке)
ApiFetcher*                     g_apiFetcher = nullptr; // Глобальный, используется в UI.cpp

// Шрифты
ImFont* g_customFont = nullptr; // symbols_skyquake.ttf (используется по требованию) - экспортируем для UI.cpp

// Состояние приложения
struct AppState {
    std::string jsonInput;
    std::string jsonOutput;
    std::string statusMessage;
    std::string errorMessage;
    bool isParsing = false;
    std::mutex stateMutex;
    
    std::vector<std::string> parseHistory;
    
    void UpdateStatus(const std::string& msg) {
        std::lock_guard<std::mutex> lock(stateMutex);
        statusMessage = msg;
    }
    
    void UpdateError(const std::string& msg) {
        std::lock_guard<std::mutex> lock(stateMutex);
        errorMessage = msg;
    }
    
    void SetParsing(bool parsing) {
        std::lock_guard<std::mutex> lock(stateMutex);
        isParsing = parsing;
    }
    
    void AddToHistory(const std::string& json) {
        std::lock_guard<std::mutex> lock(stateMutex);
        parseHistory.push_back(json);
        if (parseHistory.size() > 50) {
            parseHistory.erase(parseHistory.begin());
        }
    }
};

static AppState g_appState;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Функция для отображения JSON значения в интерфейсе
void DisplayJsonValue(std::string& output, std::shared_ptr<Json::Value> value, const std::string& prefix = "", int depth = 0) {
    if (!value) {
        output += "null";
        return;
    }
    
    std::string indent(depth * 2, ' ');
    std::ostringstream oss;
    
    switch (value->type) {
        case Json::ValueType::Null:
            output += prefix + "null";
            break;
        case Json::ValueType::Boolean:
            output += prefix + (value->asBool() ? "true" : "false");
            break;
        case Json::ValueType::Number:
            oss << std::fixed << std::setprecision(6) << value->asNumber();
            output += prefix + oss.str();
            oss.str("");
            oss.clear();
            break;
        case Json::ValueType::String:
            output += prefix + "\"" + value->asString() + "\"";
            break;
        case Json::ValueType::Array:
            output += prefix + "[\n";
            for (size_t i = 0; i < value->arrayValue.size(); ++i) {
                oss << indent << "  [" << i << "]: ";
                output += oss.str();
                oss.str("");
                oss.clear();
                DisplayJsonValue(output, value->arrayValue[i], "", depth + 1);
                if (i < value->arrayValue.size() - 1) output += ",";
                output += "\n";
            }
            output += indent + "]";
            break;
        case Json::ValueType::Object:
            output += prefix + "{\n";
            size_t idx = 0;
            for (const auto& pair : value->objectValue) {
                output += indent + "  \"" + pair.first + "\": ";
                DisplayJsonValue(output, pair.second, "", depth + 1);
                if (++idx < value->objectValue.size()) output += ",";
                output += "\n";
            }
            output += indent + "}";
            break;
    }
}

// Main code
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Привязываем главный поток к другому ядру (для интерфейса)
    // Парсер будет на первом ядре, интерфейс на втором
    #ifdef _WIN32
    DWORD_PTR processAffinityMask = 0;
    DWORD_PTR systemAffinityMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask)) {
        // Находим второе доступное ядро
        DWORD_PTR core2 = 0x2;
        if (processAffinityMask & core2) {
            SetThreadAffinityMask(GetCurrentThread(), core2);
        }
    }
    #endif
    
    // Создаем JSON парсер (будет работать на отдельном потоке/ядре)
    g_jsonParser = new JsonParser();
    
    // Создаем API загрузчик (будет работать на отдельном потоке)
    g_apiFetcher = new ApiFetcher();
    
    // Настраиваем callback'и для обработки данных
    g_apiFetcher->SetChatCallback([](const std::string& jsonData) {
        // Парсим чат в отдельном потоке
        extern void ParseGameChat(const std::string& jsonData);
        ParseGameChat(jsonData);
    });
    
    g_apiFetcher->SetEventCallback([](const std::string& jsonData) {
        // Парсим события в отдельном потоке
        extern void ParseHudMsg(const std::string& jsonData);
        ParseHudMsg(jsonData);
    });
    
    g_apiFetcher->SetIndicatorsCallback([](const std::string& jsonData) {
        // Парсим indicators в отдельном потоке
        extern void ParseIndicators(const std::string& jsonData);
        ParseIndicators(jsonData);
    });
    
    g_apiFetcher->SetStateCallback([](const std::string& jsonData) {
        // Парсим state в отдельном потоке
        extern void ParseState(const std::string& jsonData);
        ParseState(jsonData);
    });
    
    g_apiFetcher->SetMissionCallback([](const std::string& jsonData) {
        // Парсим mission в отдельном потоке
        extern void ParseMission(const std::string& jsonData);
        ParseMission(jsonData);
    });
    
    g_apiFetcher->SetMapInfoCallback([](const std::string& jsonData) {
        // Парсим map_info в отдельном потоке
        extern void ParseMapInfo(const std::string& jsonData);
        ParseMapInfo(jsonData);
    });
    
    g_apiFetcher->SetMapObjectsCallback([](const std::string& jsonData) {
        // Парсим map_obj в отдельном потоке
        extern void ParseMapObjects(const std::string& jsonData);
        ParseMapObjects(jsonData);
    });
    
    // Запускаем загрузчик
    g_apiFetcher->Start();
    
    // Make process DPI aware and obtain main monitor scale
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    
    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"WarThunderAdvanced", nullptr };
    ::RegisterClassExW(&wc);
    
    // Window size: 1280x720
    const int windowWidth = 1280;
    const int windowHeight = 720;
    
    // Окно без рамки (WS_POPUP) для кастомного title bar через ImGui
    HWND hwnd = ::CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Advanced Map War Thunder",
        WS_POPUP,
        100, 100, windowWidth, windowHeight,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    g_hWnd = hwnd;
    
    // Закругленные углы (Windows 11+)
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    
    // Темная рамка
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    
    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        delete g_jsonParser;
        return 1;
    }
    
    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Docking may not be available in this ImGui version
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    
    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    
    // Initialize UI
    InitializeUI();
    
    // Загружаем дефолтный шрифт с поддержкой кириллицы и китайских символов
    // Используем подход из старого проекта: сначала основной шрифт, затем MergeMode для китайских глифов
    
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 16.0f;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = false;
    
    // Получаем путь к папке Windows\Fonts
    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string fontsPath = std::string(winDir) + "\\Fonts\\";
    
    // Используем встроенный диапазон для кириллицы
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesCyrillic();
    
    ImFont* defaultFont = nullptr;
    
    // Пробуем разные шрифты с поддержкой кириллицы
    const char* fontFiles[] = {
        "arial.ttf",
        "tahoma.ttf", 
        "verdana.ttf",
        "segoeui.ttf",
        "times.ttf",
        "cour.ttf"
    };
    
    for (const char* fontFile : fontFiles)
    {
        std::string fullPath = fontsPath + fontFile;
        defaultFont = io.Fonts->AddFontFromFileTTF(fullPath.c_str(), 16.0f, &fontConfig, glyphRanges);
        if (defaultFont)
            break;
    }
    
    if (!defaultFont)
    {
        // Если ничего не нашли - стандартный шрифт (без кириллицы)
        defaultFont = io.Fonts->AddFontDefault(&fontConfig);
    }
    
    // === ДОБАВЛЯЕМ ГЛИФЫ ДЛЯ КИТАЙСКИХ ИЕРОГЛИФОВ (CJK) ===
    // Мержим в основной шрифт, чтобы рус/латиница остались как есть
    {
        ImFontConfig cjkConfig;
        cjkConfig.MergeMode = true;
        cjkConfig.PixelSnapH = false;
        cjkConfig.OversampleH = 2;
        cjkConfig.OversampleV = 2;
        
        // Диапазон глифов для китайского
        const ImWchar* chineseRanges = io.Fonts->GetGlyphRangesChineseFull();
        
        // Наиболее типичные китайские шрифты Windows
        const char* cjkFontFiles[] = {
            "msyh.ttc",   // Microsoft YaHei
            "msyh.ttf",
            "simhei.ttf",
            "simsun.ttc"
        };
        
        bool cjkLoaded = false;
        for (const char* fontFile : cjkFontFiles)
        {
            std::string fullPath = fontsPath + fontFile;
            ImFont* cjkFont = io.Fonts->AddFontFromFileTTF(fullPath.c_str(), 16.0f, &cjkConfig, chineseRanges);
            if (cjkFont)
            {
                cjkLoaded = true;
                break;
            }
        }
        
        // Если ни один китайский шрифт не найден — ничего страшного, просто не будет глифов
        (void)cjkLoaded;
    }
    
    // Загружаем symbols_skyquake.ttf как дополнительный шрифт (не дефолтный)
    const unsigned char* fontData = FontData::GetData();
    size_t fontDataSize = FontData::GetSize();
    
    if (fontData && fontDataSize > 0) {
        // Загружаем шрифт из памяти как дополнительный
        g_customFont = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(fontData), 
            static_cast<int>(fontDataSize), 
            16.0f, 
            nullptr, 
            io.Fonts->GetGlyphRangesDefault() // Базовые символы для custom font
        );
    }
    
    // Собираем атлас шрифтов
    io.Fonts->Build();
    
    // Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle messages
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        
        // Handle window being minimized
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;
        
        // Handle window resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }
        
        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        // Render UI
        RenderUI();
        
        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0); // VSync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }
    
    // Cleanup
    ShutdownUI();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    
    // Остановить API загрузчик
    if (g_apiFetcher) {
        g_apiFetcher->Stop();
        delete g_apiFetcher;
        g_apiFetcher = nullptr;
    }
    
    // Остановить парсер
    if (g_jsonParser) {
        g_jsonParser->Stop();
        delete g_jsonParser;
    }
    
    return 0;
}


// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
                                                 featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
                                                 &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, 
                                             featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
                                             &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;
    
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    
    // Сохраняем настройки при изменении позиции или размера окна
    if (msg == WM_WINDOWPOSCHANGED) {
        WINDOWPOS* wp = (WINDOWPOS*)lParam;
        if (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE)) {
            // Отложенное сохранение (чтобы не сохранять слишком часто)
            static DWORD lastSaveTime = 0;
            DWORD currentTime = GetTickCount();
            if (currentTime - lastSaveTime > 1000) { // Сохраняем максимум раз в секунду
                extern void SaveSettings();
                SaveSettings();
                lastSaveTime = currentTime;
            }
        }
    }
    
    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SIZING:
    {
        // Поддержание пропорций 16:9 при изменении размера
        RECT* rect = (RECT*)lParam;
        int width = rect->right - rect->left;
        int height = rect->bottom - rect->top;
        
        const float aspectRatio = 16.0f / 9.0f;
        
        // Определяем, какой край изменяется
        switch (wParam)
        {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            // Изменяется ширина - корректируем высоту
            height = (int)(width / aspectRatio);
            rect->bottom = rect->top + height;
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            // Изменяется высота - корректируем ширину
            width = (int)(height * aspectRatio);
            rect->right = rect->left + width;
            break;
        case WMSZ_TOPLEFT:
            // Левый верхний угол
            height = (int)(width / aspectRatio);
            rect->top = rect->bottom - height;
            break;
        case WMSZ_TOPRIGHT:
            // Правый верхний угол
            height = (int)(width / aspectRatio);
            rect->top = rect->bottom - height;
            break;
        case WMSZ_BOTTOMLEFT:
            // Левый нижний угол
            height = (int)(width / aspectRatio);
            rect->bottom = rect->top + height;
            break;
        case WMSZ_BOTTOMRIGHT:
            // Правый нижний угол
            height = (int)(width / aspectRatio);
            rect->bottom = rect->top + height;
            break;
        }
        return TRUE;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_NCHITTEST:
    {
        // Обработка перетаскивания окна и ресайза
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ::ScreenToClient(hWnd, &pt);
        
        RECT rc;
        ::GetClientRect(hWnd, &rc);
        
        const int borderSize = 5;
        const int titleBarHeight = 32;
        const int buttonAreaWidth = 32 * 3 + 4; // 3 кнопки
        
        // Ресайз за края
        if (pt.y < borderSize)
        {
            if (pt.x < borderSize) return HTTOPLEFT;
            if (pt.x > rc.right - borderSize) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y > rc.bottom - borderSize)
        {
            if (pt.x < borderSize) return HTBOTTOMLEFT;
            if (pt.x > rc.right - borderSize) return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        if (pt.x < borderSize) return HTLEFT;
        if (pt.x > rc.right - borderSize) return HTRIGHT;
        
        // Title bar для перетаскивания (исключая область кнопок)
        if (pt.y < titleBarHeight && pt.x < rc.right - buttonAreaWidth)
        {
            return HTCAPTION;
        }
        
        return HTCLIENT;
    }
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

