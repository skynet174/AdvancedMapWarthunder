#include "UI.h"
#include "ApiFetcher.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "Translator.h"
#include <windows.h>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winhttp.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::ComPtr;

// Внешние переменные (объявлены в main.cpp)
extern ImFont* g_customFont;
extern HWND g_hWnd;
extern ID3D11Device* g_pd3dDevice;

// Глобальные данные для чата и событий
std::vector<ChatMessage> g_chatMessages;
std::vector<EventMessage> g_eventMessages;
std::mutex g_chatMutex;
std::mutex g_eventMutex;
int g_lastChatId = 0;
int g_lastEventId = 0;

// Глобальные данные для indicators, state, mission и map_info
IndicatorsData g_indicatorsData;
StateData g_stateData;
MissionData g_missionData;
MapInfoData g_mapInfoData;
std::vector<MapObject> g_mapObjects;
std::vector<MapMarker> g_mapMarkers;
std::vector<size_t> g_selectedUnits;
std::vector<SelectedUnitInfo> g_selectedUnitsBackup;
std::mutex g_indicatorsMutex;
std::mutex g_stateMutex;
std::mutex g_missionMutex;
std::mutex g_mapInfoMutex;
std::mutex g_mapObjectsMutex;
std::mutex g_mapMarkersMutex;

// Текстура подложки
static ID3D11ShaderResourceView* g_backgroundTexture = nullptr;
static int g_backgroundWidth = 2048;
static int g_backgroundHeight = 2048;
static int g_lastMapGeneration = -1; // Для отслеживания смены карты

// Настройки приложения
static bool g_content2Visible = false; // Видимость чата (по умолчанию выключен)
static bool g_debugMode = false; // Режим отладки (клавиша D)
static bool g_followMode = false; // Режим слежения (клавиша F)
static float g_followZoomAdjust = 1.0f; // Корректировка зума при слежении
static bool g_wasDragging = false; // Флаг для отслеживания drag (чтобы не ставить метку после перемещения)

// Параметры камеры для карты (pan & zoom) с инерцией
static float g_mapZoom = 1.0f;
static float g_mapOffsetX = 0.0f;
static float g_mapOffsetY = 0.0f;
static float g_targetZoom = 1.0f;
static float g_targetOffsetX = 0.0f;
static float g_targetOffsetY = 0.0f;

// Скорости для инерционной интерполяции (только для offset, не для зума)
static float g_offsetXVelocity = 0.0f;
static float g_offsetYVelocity = 0.0f;

// Параметры инерции
static const float g_friction = 0.85f; // Коэффициент трения (0.85 = 15% потери скорости за кадр)
static const float g_minVelocity = 0.01f; // Минимальная скорость для остановки

// Функция для парсинга hex цвета в ImVec4
ImVec4 ParseColorHex(const std::string& hexColor) {
    // Формат: #AARRGGBB (ARGB) или #RRGGBB (RGB)
    if (hexColor.length() < 7 || hexColor[0] != '#') {
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Белый по умолчанию
    }
    
    unsigned int a = 255, r = 255, g = 255, b = 255;
    
    if (hexColor.length() >= 9) {
        // Формат #AARRGGBB (ARGB - 8 символов после #)
        sscanf_s(hexColor.c_str() + 1, "%02x%02x%02x%02x", &a, &r, &g, &b);
    } else if (hexColor.length() >= 7) {
        // Формат #RRGGBB (RGB - 6 символов после #)
        sscanf_s(hexColor.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
        a = 255; // Полная непрозрачность по умолчанию
    }
    
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// Функция для загрузки данных из URL
bool DownloadDataFromURL(const std::string& url, std::vector<unsigned char>& outData)
{
    outData.clear();

    // Парсим URL
    // Ожидаем формат: http://host:port/path
    std::string host = "127.0.0.1";
    int port = 8111;
    std::string path = "/";

    // Простой парсинг URL
    size_t hostStart = url.find("://");
    if (hostStart != std::string::npos)
    {
        hostStart += 3;
        size_t portStart = url.find(":", hostStart);
        size_t pathStart = url.find("/", hostStart);
        
        if (portStart != std::string::npos && portStart < pathStart)
        {
            host = url.substr(hostStart, portStart - hostStart);
            port = std::stoi(url.substr(portStart + 1, pathStart - portStart - 1));
        }
        else if (pathStart != std::string::npos)
        {
            host = url.substr(hostStart, pathStart - hostStart);
        }
        
        if (pathStart != std::string::npos)
            path = url.substr(pathStart);
    }

    // Конвертируем в wide string
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());

    // Открываем сессию
    HINTERNET hSession = WinHttpOpen(
        L"WarThunderAdvanced/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession)
        return false;

    // Подключаемся
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Открываем запрос
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0
    );

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Таймаут
    DWORD timeout = 5000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    // Отправляем
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Получаем ответ
    if (!WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Читаем данные
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;

    do
    {
        bytesAvailable = 0;
        WinHttpQueryDataAvailable(hRequest, &bytesAvailable);

        if (bytesAvailable > 0)
        {
            size_t offset = outData.size();
            outData.resize(offset + bytesAvailable);
            WinHttpReadData(hRequest, outData.data() + offset, bytesAvailable, &bytesRead);
        }
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return !outData.empty();
}

// Функция для загрузки текстуры из памяти с растягиванием до указанного размера
bool LoadTextureFromMemory(const void* data, size_t size, ID3D11ShaderResourceView** outSRV, int targetWidth, int targetHeight)
{
    if (!g_pd3dDevice || !outSRV || !data || size == 0)
        return false;

    *outSRV = nullptr;

    // Инициализируем COM (если ещё не инициализирован)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Создаём WIC factory
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );

    if (FAILED(hr))
        return false;

    // Создаём stream из памяти
    ComPtr<IWICStream> stream;
    hr = wicFactory->CreateStream(&stream);
    if (FAILED(hr))
        return false;

    hr = stream->InitializeFromMemory((BYTE*)data, (DWORD)size);
    if (FAILED(hr))
        return false;

    // Создаём decoder
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );

    if (FAILED(hr))
        return false;

    // Получаем первый frame
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        return false;

    // Получаем исходные размеры
    UINT originalWidth, originalHeight;
    frame->GetSize(&originalWidth, &originalHeight);

    // Конвертируем в BGRA (стандартный формат для DirectX)
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return false;

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom
    );

    if (FAILED(hr))
        return false;

    // Растягиваем до целевого размера (2048x2048)
    ComPtr<IWICBitmapScaler> scaler;
    hr = wicFactory->CreateBitmapScaler(&scaler);
    if (FAILED(hr))
        return false;

    hr = scaler->Initialize(
        converter.Get(),
        targetWidth,
        targetHeight,
        WICBitmapInterpolationModeLinear
    );

    if (FAILED(hr))
        return false;

    // Копируем пиксели из растянутого изображения
    UINT stride = targetWidth * 4;
    UINT bufferSize = stride * targetHeight;
    std::vector<BYTE> pixels(bufferSize);

    hr = scaler->CopyPixels(nullptr, stride, bufferSize, pixels.data());
    if (FAILED(hr))
        return false;

    // Создаём текстуру D3D11 (BGRA формат)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = targetWidth;
    texDesc.Height = targetHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    hr = g_pd3dDevice->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr))
        return false;

    // Создаём Shader Resource View
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_pd3dDevice->CreateShaderResourceView(texture.Get(), &srvDesc, outSRV);
    if (FAILED(hr))
        return false;

    return true;
}

// Функция для загрузки PNG текстуры из файла
bool LoadTextureFromFile(const std::wstring& path, ID3D11ShaderResourceView** outSRV, int* outWidth, int* outHeight)
{
    if (!g_pd3dDevice || !outSRV)
        return false;

    *outSRV = nullptr;

    // Инициализируем COM (если ещё не инициализирован)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Создаём WIC factory
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );

    if (FAILED(hr))
        return false;

    // Читаем файл
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    std::vector<unsigned char> data(fileSize);

    DWORD bytesRead;
    ReadFile(hFile, data.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);

    // Создаём stream из памяти
    ComPtr<IWICStream> stream;
    hr = wicFactory->CreateStream(&stream);
    if (FAILED(hr))
        return false;

    hr = stream->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size());
    if (FAILED(hr))
        return false;

    // Создаём decoder
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );

    if (FAILED(hr))
        return false;

    // Получаем первый frame
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        return false;

    // Получаем размеры
    UINT width, height;
    frame->GetSize(&width, &height);

    if (outWidth) *outWidth = (int)width;
    if (outHeight) *outHeight = (int)height;

    // Конвертируем в BGRA (стандартный формат для DirectX)
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return false;

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom
    );

    if (FAILED(hr))
        return false;

    // Копируем пиксели
    UINT stride = width * 4;
    UINT bufferSize = stride * height;
    std::vector<BYTE> pixels(bufferSize);

    hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
    if (FAILED(hr))
        return false;

    // Создаём текстуру D3D11 (BGRA формат)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    hr = g_pd3dDevice->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr))
        return false;

    // Создаём Shader Resource View
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_pd3dDevice->CreateShaderResourceView(texture.Get(), &srvDesc, outSRV);
    if (FAILED(hr))
        return false;

    return true;
}

// Функция для проверки, является ли символ валидным hex-символом
static bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || 
           (c >= 'A' && c <= 'F') || 
           (c >= 'a' && c <= 'f');
}

// Функция для проверки валидности цветового кода
static bool IsValidColorCode(const std::string& text, size_t pos, size_t& colorLength) {
    if (pos >= text.length() || text[pos] != '#') {
        return false;
    }
    
    // Проверяем формат #RRGGBB (7 символов: # + 6 hex)
    if (pos + 7 <= text.length()) {
        bool isValid = true;
        for (size_t i = pos + 1; i < pos + 7; i++) {
            if (!IsHexChar(text[i])) {
                isValid = false;
                break;
            }
        }
        if (isValid) {
            colorLength = 7;
            return true;
        }
    }
    
    // Проверяем формат #AARRGGBB (9 символов: # + 8 hex)
    if (pos + 9 <= text.length()) {
        bool isValid = true;
        for (size_t i = pos + 1; i < pos + 9; i++) {
            if (!IsHexChar(text[i])) {
                isValid = false;
                break;
            }
        }
        if (isValid) {
            colorLength = 9;
            return true;
        }
    }
    
    return false;
}

// Функция для отображения текста с поддержкой цветовых кодов #RRGGBB или #AARRGGBB
// Формат: #RRGGBBтекст или обычный текст#RRGGBBтекст
// Также поддерживает теги: <color=#RRGGBB>текст</color>
void RenderColoredText(const std::string& text) {
    if (text.empty()) return;
    
    // Сохраняем начальную позицию X и Y для правильного переноса
    float startPosX = ImGui::GetCursorPosX();
    float startPosY = ImGui::GetCursorPosY();
    float wrapPos = ImGui::GetContentRegionAvail().x;
    
    // Получаем высоту строки текста для учета отступа
    float lineHeight = ImGui::GetTextLineHeight();
    
    // Устанавливаем перенос текста с учетом начальной позиции
    if (wrapPos > 0.0f) {
        ImGui::PushTextWrapPos(startPosX + wrapPos);
    }
    
    size_t pos = 0;
    bool isFirstPart = true;
    
    while (pos < text.length()) {
        // Ищем оба варианта: тег <color= и hex цвет #
        size_t colorTagStart = text.find("<color=", pos);
        size_t hashPos = text.find("#", pos);
        
        // Определяем, что обрабатывать первым (ближайшее к текущей позиции)
        bool processTag = false;
        size_t nextPos = text.length();
        
        if (colorTagStart != std::string::npos && colorTagStart < text.length()) {
            // Проверяем, не является ли найденный # частью тега <color=
            if (hashPos != std::string::npos && hashPos >= colorTagStart && hashPos < colorTagStart + 20) {
                // # находится внутри или рядом с тегом, обрабатываем тег
                processTag = true;
            } else if (hashPos == std::string::npos || colorTagStart < hashPos) {
                // Тег ближе, чем hex цвет
                processTag = true;
            }
        }
        
        if (processTag && colorTagStart != std::string::npos) {
            // Обрабатываем тег <color=#...>
            size_t colorValueStart = colorTagStart + 7; // 7 = длина "<color="
            
            // Ищем закрывающую скобку >
            size_t colorTagEnd = text.find(">", colorValueStart);
            if (colorTagEnd != std::string::npos) {
                // Извлекаем цвет из атрибута (формат: <color=#RRGGBB> или <color=#AARRGGBB>)
                std::string colorAttr = text.substr(colorValueStart, colorTagEnd - colorValueStart);
                
                // Проверяем, начинается ли с #
                if (colorAttr.length() > 0 && colorAttr[0] == '#') {
                    size_t colorLength = 0;
                    if (IsValidColorCode(colorAttr, 0, colorLength)) {
                        // Валидный цветовой код в теге
                        std::string colorHex = colorAttr.substr(0, colorLength);
                        ImVec4 color = ParseColorHex(colorHex);
                        
                        // Ищем закрывающий тег </color>
                        size_t contentStart = colorTagEnd + 1; // После >
                        size_t closeTagStart = text.find("</color>", contentStart);
                        
                        if (closeTagStart != std::string::npos) {
                            // Выводим текст до открывающего тега
                            if (colorTagStart > pos) {
                                std::string beforeTag = text.substr(pos, colorTagStart - pos);
                                if (!isFirstPart) {
                                    ImGui::SameLine(0, 0);
                                }
                                ImGui::TextWrapped("%s", beforeTag.c_str());
                                isFirstPart = false;
                            }
                            
                            // Выводим цветной текст между тегами
                            std::string coloredText = text.substr(contentStart, closeTagStart - contentStart);
                            if (!isFirstPart) {
                                ImGui::SameLine(0, 0);
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, color);
                            ImGui::TextWrapped("%s", coloredText.c_str());
                            ImGui::PopStyleColor();
                            isFirstPart = false;
                            
                            // Продолжаем после закрывающего тега
                            pos = closeTagStart + 8; // 8 = длина "</color>"
                            continue;
                        }
                    }
                }
            }
            // Если тег невалиден, продолжаем поиск с позиции после тега
            pos = colorTagStart + 1;
            continue;
        }
        
        // Обрабатываем hex цвет #RRGGBB
        if (hashPos == std::string::npos) {
            // Нет больше символов #, выводим оставшийся текст
            if (pos < text.length()) {
                std::string remaining = text.substr(pos);
                if (!isFirstPart) {
                    ImGui::SameLine(0, 0);
                }
                ImGui::TextWrapped("%s", remaining.c_str());
            }
            break;
        }
        
        // Выводим текст до символа #
        if (hashPos > pos) {
            std::string beforeHash = text.substr(pos, hashPos - pos);
            if (!isFirstPart) {
                ImGui::SameLine(0, 0);
            }
            ImGui::TextWrapped("%s", beforeHash.c_str());
            isFirstPart = false;
        }
        
        // Проверяем, является ли это валидным цветовым кодом
        size_t colorLength = 0;
        if (IsValidColorCode(text, hashPos, colorLength)) {
            // Это валидный цветовой код
            std::string colorHex = text.substr(hashPos, colorLength);
            ImVec4 color = ParseColorHex(colorHex);
            
            // Ищем конец цветного текста (следующий валидный # или тег <color= или конец строки)
            size_t textStart = hashPos + colorLength;
            size_t textEnd = text.length();
            
            // Ищем следующий валидный цветовой код или тег <color=
            for (size_t i = textStart; i < text.length(); i++) {
                if (text[i] == '#') {
                    size_t nextColorLength = 0;
                    if (IsValidColorCode(text, i, nextColorLength)) {
                        textEnd = i;
                        break;
                    }
                } else if (i + 7 <= text.length() && text.substr(i, 7) == "<color=") {
                    textEnd = i;
                    break;
                }
            }
            
            // Выводим цветной текст
            if (textStart < textEnd) {
                std::string coloredText = text.substr(textStart, textEnd - textStart);
                if (!isFirstPart) {
                    ImGui::SameLine(0, 0);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s", coloredText.c_str());
                ImGui::PopStyleColor();
                isFirstPart = false;
                
                // Если это не последняя часть, используем SameLine для следующей части
                bool isLastPart = (textEnd >= text.length());
                if (!isLastPart) {
                    ImGui::SameLine(0, 0);
                }
            }
            
            pos = textEnd;
        } else {
            // Это не валидный цветовой код, выводим # как обычный символ
            if (!isFirstPart) {
                ImGui::SameLine(0, 0);
            }
            ImGui::TextUnformatted("#");
            isFirstPart = false;
            pos = hashPos + 1;
        }
    }
    
    if (wrapPos > 0.0f) {
        ImGui::PopTextWrapPos();
    }
}

// Парсинг игрового чата
void ParseGameChat(const std::string& jsonData) {
    // Простой парсинг JSON массива сообщений
    // Формат: [{"id": 70, "msg": "...", "sender": "...", "enemy": false, "mode": "All"}, ...]
    
    std::lock_guard<std::mutex> lock(g_chatMutex);
    
    // Функция для поиска конца объекта (с учетом вложенных объектов и строк)
    auto findObjectEnd = [](const std::string& str, size_t start) -> size_t {
        int depth = 0;
        bool inString = false;
        for (size_t i = start; i < str.size(); i++) {
            if (str[i] == '"' && (i == 0 || str[i-1] != '\\'))
                inString = !inString;
            if (!inString) {
                if (str[i] == '{') depth++;
                if (str[i] == '}') {
                    depth--;
                    if (depth == 0) return i + 1;
                }
            }
        }
        return std::string::npos;
    };
    
    size_t pos = 0;
    while ((pos = jsonData.find("{", pos)) != std::string::npos) {
        size_t objEnd = findObjectEnd(jsonData, pos);
        if (objEnd == std::string::npos) break;
        
        std::string objStr = jsonData.substr(pos, objEnd - pos);
        pos = objEnd;
        
        ChatMessage msg;
        
        // Используем правильный парсинг JSON (паттерн из старого проекта)
        // Функция для парсинга int из JSON
        auto parseJsonInt = [](const std::string& str, const std::string& key) -> int {
            size_t pos = str.find("\"" + key + "\"");
            if (pos != std::string::npos) {
                pos = str.find(":", pos);
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                        pos++;
                    return std::stoi(str.substr(pos));
                }
            }
            return 0;
        };
        
        // Функция для парсинга boolean из JSON
        auto parseJsonBool = [](const std::string& str, const std::string& key) -> bool {
            size_t pos = str.find("\"" + key + "\"");
            if (pos != std::string::npos) {
                pos = str.find(":", pos);
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                        pos++;
                    // Проверяем "true" или "false"
                    if (pos + 4 <= str.size() && str.substr(pos, 4) == "true")
                        return true;
                    if (pos + 5 <= str.size() && str.substr(pos, 5) == "false")
                        return false;
                }
            }
            return false;
        };
        
        // Функция для парсинга string из JSON (с правильной обработкой escape-последовательностей)
        auto parseJsonString = [](const std::string& str, const std::string& key) -> std::string {
            size_t pos = str.find("\"" + key + "\"");
            if (pos != std::string::npos) {
                pos = str.find(":", pos);
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\"'))
                        pos++;
                    size_t end = pos;
                    while (end < str.size()) {
                        if (str[end] == '\\' && end + 1 < str.size()) {
                            end += 2; // Пропускаем escape-последовательность
                            continue;
                        }
                        if (str[end] == '"')
                            break;
                        end++;
                    }
                    std::string result = str.substr(pos, end - pos);
                    // Раскодируем escape-последовательности
                    std::string decoded;
                    for (size_t i = 0; i < result.size(); i++) {
                        if (result[i] == '\\' && i + 1 < result.size()) {
                            if (result[i+1] == 'n') { decoded += '\n'; i++; }
                            else if (result[i+1] == 't') { decoded += '\t'; i++; }
                            else if (result[i+1] == 'r') { decoded += '\r'; i++; }
                            else if (result[i+1] == '\\') { decoded += '\\'; i++; }
                            else if (result[i+1] == '"') { decoded += '"'; i++; }
                            else decoded += result[i];
                        } else {
                            decoded += result[i];
                        }
                    }
                    return decoded;
                }
            }
            return "";
        };
        
        msg.id = parseJsonInt(objStr, "id");
        msg.msg = parseJsonString(objStr, "msg");
        msg.sender = parseJsonString(objStr, "sender");
        msg.enemy = parseJsonBool(objStr, "enemy"); // Правильный парсинг boolean
        msg.mode = parseJsonString(objStr, "mode");
        
        if (msg.id > g_lastChatId) {
            g_lastChatId = msg.id;
            extern ApiFetcher* g_apiFetcher;
            if (g_apiFetcher) {
                g_apiFetcher->SetLastChatId(msg.id);
            }
        }
        
        // Добавляем сообщение (проверяем, нет ли уже такого ID)
        bool exists = false;
        for (const auto& existing : g_chatMessages) {
            if (existing.id == msg.id) {
                exists = true;
                break;
            }
        }
        if (!exists && !msg.msg.empty()) {
            g_chatMessages.push_back(msg);
            // Ограничиваем размер (последние 200 сообщений)
            if (g_chatMessages.size() > 200) {
                g_chatMessages.erase(g_chatMessages.begin());
            }
        }
    }
}

// Парсинг событий (hudmsg) - используем паттерн из старого проекта
void ParseHudMsg(const std::string& jsonData) {
    // Формат: {"events": [], "damage": [{"id": 161, "msg": "...", "sender": "...", "enemy": false, "mode": ""}, ...]}
    
    std::lock_guard<std::mutex> lock(g_eventMutex);
    
    // Ищем массив damage
    size_t damageStart = jsonData.find("\"damage\":[");
    if (damageStart == std::string::npos) return;
    
    damageStart += 10; // Пропускаем "damage":[
    size_t damageEnd = jsonData.find("]", damageStart);
    if (damageEnd == std::string::npos) return;
    
    std::string damageArray = jsonData.substr(damageStart, damageEnd - damageStart);
    
    // Функция для поиска конца объекта (с учетом вложенных объектов и строк)
    auto findObjectEnd = [](const std::string& str, size_t start) -> size_t {
        int depth = 0;
        bool inString = false;
        for (size_t i = start; i < str.size(); i++) {
            if (str[i] == '"' && (i == 0 || str[i-1] != '\\'))
                inString = !inString;
            if (!inString) {
                if (str[i] == '{') depth++;
                if (str[i] == '}') {
                    depth--;
                    if (depth == 0) return i + 1;
                }
            }
        }
        return std::string::npos;
    };
    
    // Функция для парсинга int из JSON
    auto parseJsonInt = [](const std::string& str, const std::string& key) -> int {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                return std::stoi(str.substr(pos));
            }
        }
        return 0;
    };
    
    // Функция для парсинга string из JSON (с правильной обработкой escape-последовательностей)
    auto parseJsonString = [](const std::string& str, const std::string& key) -> std::string {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\"'))
                    pos++;
                size_t end = pos;
                while (end < str.size()) {
                    if (str[end] == '\\' && end + 1 < str.size()) {
                        end += 2; // Пропускаем escape-последовательность
                        continue;
                    }
                    if (str[end] == '"')
                        break;
                    end++;
                }
                std::string result = str.substr(pos, end - pos);
                // Раскодируем escape-последовательности
                std::string decoded;
                for (size_t i = 0; i < result.size(); i++) {
                    if (result[i] == '\\' && i + 1 < result.size()) {
                        if (result[i+1] == 'n') { decoded += '\n'; i++; }
                        else if (result[i+1] == 't') { decoded += '\t'; i++; }
                        else if (result[i+1] == 'r') { decoded += '\r'; i++; }
                        else if (result[i+1] == '\\') { decoded += '\\'; i++; }
                        else if (result[i+1] == '"') { decoded += '"'; i++; }
                        else decoded += result[i];
                    } else {
                        decoded += result[i];
                    }
                }
                return decoded;
            }
        }
        return "";
    };
    
    // Сохраняем все объекты для поиска предыдущих сообщений
    std::vector<std::pair<size_t, size_t>> objectPositions; // (start, end)
    size_t tempPos = 0;
    while ((tempPos = damageArray.find("{", tempPos)) != std::string::npos) {
        size_t objEnd = findObjectEnd(damageArray, tempPos);
        if (objEnd == std::string::npos) break;
        objectPositions.push_back({tempPos, objEnd});
        tempPos = objEnd;
    }
    
    size_t objPos = 0;
    size_t currentObjIndex = 0;
    while ((objPos = damageArray.find("{", objPos)) != std::string::npos) {
        size_t objEnd = findObjectEnd(damageArray, objPos);
        if (objEnd == std::string::npos) break;
        
        std::string objStr = damageArray.substr(objPos, objEnd - objPos);
        size_t originalObjPos = objPos;
        objPos = objEnd;
        
        // Функция для парсинга boolean из JSON
        auto parseJsonBool = [](const std::string& str, const std::string& key) -> bool {
            size_t pos = str.find("\"" + key + "\"");
            if (pos != std::string::npos) {
                pos = str.find(":", pos);
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                        pos++;
                    // Проверяем "true" или "false"
                    if (pos + 4 <= str.size() && str.substr(pos, 4) == "true")
                        return true;
                    if (pos + 5 <= str.size() && str.substr(pos, 5) == "false")
                        return false;
                }
            }
            return false;
        };
        
        int id = parseJsonInt(objStr, "id");
        std::string msg = parseJsonString(objStr, "msg");
        std::string sender = parseJsonString(objStr, "sender");
        bool enemy = parseJsonBool(objStr, "enemy"); // Правильный парсинг boolean
        std::string mode = parseJsonString(objStr, "mode");
        int time = parseJsonInt(objStr, "time");
        
        // Обновляем последний обработанный ID
        if (id > g_lastEventId) {
            g_lastEventId = id;
            // Обновляем ID в ApiFetcher
            extern ApiFetcher* g_apiFetcher;
            if (g_apiFetcher) {
                g_apiFetcher->SetLastEventId(id);
            }
        }
        
        // Обработка kd?reason сообщений (паттерн из старого проекта)
        if (msg.find("kd?") != std::string::npos || msg.find("потерял связь") != std::string::npos) {
            // Проверяем, если сообщение само содержит kd?NET_PLAYER_DISCONNECT_FROM_GAME
            size_t kdPos = msg.find("kd?");
            if (kdPos != std::string::npos) {
                size_t reasonStart = kdPos + 3;
                std::string reason = msg.substr(reasonStart);
                
                // Извлекаем имя игрока: сначала из начала сообщения (до kd?), затем из sender
                std::string playerName;
                
                // Пытаемся извлечь имя из начала сообщения (до kd?)
                if (kdPos > 0) {
                    playerName = msg.substr(0, kdPos);
                    // Убираем пробелы в начале и конце
                    while (!playerName.empty() && (playerName[0] == ' ' || playerName[0] == '\t')) {
                        playerName = playerName.substr(1);
                    }
                    while (!playerName.empty() && (playerName.back() == ' ' || playerName.back() == '\t')) {
                        playerName.pop_back();
                    }
                }
                
                // Если не нашли в начале сообщения, пробуем из sender
                if (playerName.empty() && !sender.empty()) {
                    playerName = sender;
                }
                
                // Если все еще пусто, пробуем найти td!
                if (playerName.empty() && msg.find("td!") != std::string::npos) {
                    size_t nameEnd = msg.find("td!");
                    if (nameEnd != std::string::npos && nameEnd < kdPos) {
                        playerName = msg.substr(0, nameEnd);
                    }
                }
                
                // Если имя все еще не найдено, ищем в предыдущих сообщениях
                if (playerName.empty() && currentObjIndex > 0) {
                    for (int i = currentObjIndex - 1; i >= 0 && i >= (int)currentObjIndex - 3; i--) {
                        std::string prevObjStr = damageArray.substr(objectPositions[i].first, 
                                                                    objectPositions[i].second - objectPositions[i].first);
                        std::string prevMsg = parseJsonString(prevObjStr, "msg");
                        std::string prevSender = parseJsonString(prevObjStr, "sender");
                        
                        // Если предыдущее сообщение содержит имя (не kd? и не пустое)
                        if (!prevMsg.empty() && prevMsg.find("kd?") == std::string::npos) {
                            // Пробуем извлечь имя из предыдущего сообщения
                            if (!prevSender.empty()) {
                                playerName = prevSender;
                                break;
                            } else if (prevMsg.find("td!") != std::string::npos) {
                                size_t nameEnd = prevMsg.find("td!");
                                if (nameEnd != std::string::npos) {
                                    playerName = prevMsg.substr(0, nameEnd);
                                    // Убираем пробелы
                                    while (!playerName.empty() && (playerName[0] == ' ' || playerName[0] == '\t')) {
                                        playerName = playerName.substr(1);
                                    }
                                    while (!playerName.empty() && (playerName.back() == ' ' || playerName.back() == '\t')) {
                                        playerName.pop_back();
                                    }
                                    if (!playerName.empty()) break;
                                }
                            } else if (!prevMsg.empty() && prevMsg.length() < 50) {
                                // Если сообщение короткое и не содержит специальных символов, возможно это имя
                                playerName = prevMsg;
                                // Убираем пробелы
                                while (!playerName.empty() && (playerName[0] == ' ' || playerName[0] == '\t')) {
                                    playerName = playerName.substr(1);
                                }
                                while (!playerName.empty() && (playerName.back() == ' ' || playerName.back() == '\t')) {
                                    playerName.pop_back();
                                }
                                if (!playerName.empty() && playerName.find(" ") == std::string::npos) {
                                    break; // Имя обычно одно слово
                                } else {
                                    playerName.clear();
                                }
                            }
                        }
                    }
                }
                
                // Форматируем сообщение
                if (reason == "NET_PLAYER_DISCONNECT_FROM_GAME") {
                    if (!playerName.empty()) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), TR().Get("player_disconnected_fmt").c_str(), playerName.c_str());
                        msg = buf;
                    }
                    else {
                        msg = TR().Get("player_disconnected_no_name");
                    }
                } else {
                    // Заменяем подчеркивания на пробелы и делаем первую букву заглавной
                    std::string formattedReason = reason;
                    for (size_t i = 0; i < formattedReason.size(); i++) {
                        if (formattedReason[i] == '_')
                            formattedReason[i] = ' ';
                        else if (i == 0)
                            formattedReason[i] = std::toupper(formattedReason[i]);
                    }
                    if (!playerName.empty()) {
                        msg = playerName + ": " + formattedReason;
                    } else {
                        msg = formattedReason;
                    }
                }
            }
            else if (msg.find("потерял связь") != std::string::npos) {
                // Извлекаем имя игрока
                std::string playerName;
                if (msg.find("td!") != std::string::npos) {
                    size_t nameEnd = msg.find("td!");
                    if (nameEnd != std::string::npos)
                        playerName = msg.substr(0, nameEnd);
                }
                else {
                    size_t nameEnd = msg.find(" потерял связь");
                    if (nameEnd != std::string::npos)
                        playerName = msg.substr(0, nameEnd);
                }
                
                // Ищем следующее сообщение с kd?reason
                bool foundReason = false;
                size_t nextObjPos = objPos;
                while ((nextObjPos = damageArray.find("{", nextObjPos)) != std::string::npos) {
                    size_t nextObjEnd = findObjectEnd(damageArray, nextObjPos);
                    if (nextObjEnd == std::string::npos) break;
                    
                    std::string nextObjStr = damageArray.substr(nextObjPos, nextObjEnd - nextObjPos);
                    std::string nextMsg = parseJsonString(nextObjStr, "msg");
                    
                    if (nextMsg.find("kd?") != std::string::npos && (playerName.empty() || nextMsg.find(playerName) != std::string::npos)) {
                        // Извлекаем причину
                        size_t reasonStart = nextMsg.find("kd?");
                        if (reasonStart != std::string::npos) {
                            reasonStart += 3;
                            std::string reason = nextMsg.substr(reasonStart);
                            
                            // Форматируем сообщение
                            if (reason == "NET_PLAYER_DISCONNECT_FROM_GAME") {
                                if (!playerName.empty()) {
                                    char buf[256];
                                    snprintf(buf, sizeof(buf), TR().Get("player_disconnected_fmt").c_str(), playerName.c_str());
                                    msg = buf;
                                } else {
                                    msg = TR().Get("player_disconnected_no_name");
                                }
                            } else {
                                // Заменяем подчеркивания на пробелы и делаем первую букву заглавной
                                std::string formattedReason = reason;
                                for (size_t i = 0; i < formattedReason.size(); i++) {
                                    if (formattedReason[i] == '_')
                                        formattedReason[i] = ' ';
                                    else if (i == 0)
                                        formattedReason[i] = std::toupper(formattedReason[i]);
                                }
                                if (!playerName.empty()) {
                                    msg = playerName + ": " + formattedReason;
                                } else {
                                    msg = formattedReason;
                                }
                            }
                            
                            // Пропускаем следующее сообщение
                            objPos = nextObjEnd;
                            foundReason = true;
                            break;
                        }
                    }
                    nextObjPos = nextObjEnd;
                }
                
                if (!foundReason && !playerName.empty()) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), TR().Get("player_lost_connection_fmt").c_str(), playerName.c_str());
                    msg = buf;
                }
                else if (!foundReason && playerName.empty()) {
                    msg = TR().Get("player_lost_connection_no_name");
                }
            }
        }
        
        // Добавляем событие (проверяем, нет ли уже такого ID)
        bool exists = false;
        for (const auto& existing : g_eventMessages) {
            if (existing.id == id) {
                exists = true;
                break;
            }
        }
        if (!exists && !msg.empty()) {
            EventMessage eventMsg;
            eventMsg.id = id;
            eventMsg.msg = msg;
            eventMsg.sender = sender;
            eventMsg.enemy = enemy;
            eventMsg.mode = mode;
            
            g_eventMessages.push_back(eventMsg);
            // Ограничиваем размер (последние 200 событий)
            if (g_eventMessages.size() > 200) {
                g_eventMessages.erase(g_eventMessages.begin());
            }
        }
        
        currentObjIndex++;
    }
}

// Парсинг данных indicators
void ParseIndicators(const std::string& jsonData) {
    std::lock_guard<std::mutex> lock(g_indicatorsMutex);
    
    // Функция для парсинга bool из JSON
    auto parseJsonBool = [](const std::string& str, const std::string& key) -> bool {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                if (pos + 4 <= str.size() && str.substr(pos, 4) == "true")
                    return true;
                if (pos + 5 <= str.size() && str.substr(pos, 5) == "false")
                    return false;
            }
        }
        return false;
    };
    
    // Функция для парсинга string из JSON
    auto parseJsonString = [](const std::string& str, const std::string& key) -> std::string {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\"'))
                    pos++;
                size_t end = pos;
                while (end < str.size()) {
                    if (str[end] == '\\' && end + 1 < str.size()) {
                        end += 2;
                        continue;
                    }
                    if (str[end] == '"')
                        break;
                    end++;
                }
                return str.substr(pos, end - pos);
            }
        }
        return "";
    };
    
    // Функция для парсинга float из JSON
    auto parseJsonFloat = [](const std::string& str, const std::string& key) -> float {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                // Находим конец числа (до запятой, закрывающей скобки или пробела)
                size_t end = pos;
                while (end < str.size()) {
                    char c = str[end];
                    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        break;
                    end++;
                }
                if (end > pos) {
                    std::string numStr = str.substr(pos, end - pos);
                    return static_cast<float>(std::atof(numStr.c_str()));
                }
            }
        }
        return 0.0f;
    };
    
    g_indicatorsData.valid = parseJsonBool(jsonData, "valid");
    g_indicatorsData.type = parseJsonString(jsonData, "type");
    g_indicatorsData.speed = parseJsonFloat(jsonData, "speed");
    g_indicatorsData.altitude_hour = parseJsonFloat(jsonData, "altitude_hour");
    g_indicatorsData.altitude_min = parseJsonFloat(jsonData, "altitude_min");
    g_indicatorsData.compass = parseJsonFloat(jsonData, "compass");
    g_indicatorsData.mach = parseJsonFloat(jsonData, "mach");
    g_indicatorsData.g_meter = parseJsonFloat(jsonData, "g_meter");
    g_indicatorsData.fuel = parseJsonFloat(jsonData, "fuel");
    g_indicatorsData.throttle = parseJsonFloat(jsonData, "throttle");
    g_indicatorsData.gears = parseJsonFloat(jsonData, "gears");
    g_indicatorsData.flaps = parseJsonFloat(jsonData, "flaps");
}

// Парсинг данных state
void ParseState(const std::string& jsonData) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    
    // Функция для парсинга bool из JSON
    auto parseJsonBool = [](const std::string& str, const std::string& key) -> bool {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                if (pos + 4 <= str.size() && str.substr(pos, 4) == "true")
                    return true;
                if (pos + 5 <= str.size() && str.substr(pos, 5) == "false")
                    return false;
            }
        }
        return false;
    };
    
    // Функция для парсинга int из JSON
    auto parseJsonInt = [](const std::string& str, const std::string& key) -> int {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                // Находим конец числа (до запятой, закрывающей скобки или пробела)
                size_t end = pos;
                while (end < str.size()) {
                    char c = str[end];
                    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        break;
                    end++;
                }
                if (end > pos) {
                    std::string numStr = str.substr(pos, end - pos);
                    return std::atoi(numStr.c_str());
                }
            }
        }
        return 0;
    };
    
    // Функция для парсинга float из JSON
    auto parseJsonFloat = [](const std::string& str, const std::string& key) -> float {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                // Находим конец числа (до запятой, закрывающей скобки или пробела)
                size_t end = pos;
                while (end < str.size()) {
                    char c = str[end];
                    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        break;
                    end++;
                }
                if (end > pos) {
                    std::string numStr = str.substr(pos, end - pos);
                    return static_cast<float>(std::atof(numStr.c_str()));
                }
            }
        }
        return 0.0f;
    };
    
    g_stateData.valid = parseJsonBool(jsonData, "valid");
    g_stateData.altitude = parseJsonInt(jsonData, "H, m");
    g_stateData.tas = parseJsonInt(jsonData, "TAS, km/h");
    g_stateData.ias = parseJsonInt(jsonData, "IAS, km/h");
    g_stateData.mach = parseJsonFloat(jsonData, "M");
    g_stateData.aoa = parseJsonFloat(jsonData, "AoA, deg");
    g_stateData.vy = parseJsonFloat(jsonData, "Vy, m/s");
    g_stateData.fuel = parseJsonInt(jsonData, "Mfuel, kg");
    g_stateData.fuel0 = parseJsonInt(jsonData, "Mfuel0, kg");
    g_stateData.throttle1 = parseJsonInt(jsonData, "throttle 1, %");
    g_stateData.rpm1 = parseJsonInt(jsonData, "RPM 1");
    g_stateData.power1 = parseJsonFloat(jsonData, "power 1, hp");
}

// Парсинг данных mission
void ParseMission(const std::string& jsonData) {
    std::lock_guard<std::mutex> lock(g_missionMutex);
    
    // Функция для парсинга bool из JSON
    auto parseJsonBool = [](const std::string& str, const std::string& key) -> bool {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                if (pos + 4 <= str.size() && str.substr(pos, 4) == "true")
                    return true;
                if (pos + 5 <= str.size() && str.substr(pos, 5) == "false")
                    return false;
            }
        }
        return false;
    };
    
    // Функция для парсинга string из JSON
    auto parseJsonString = [](const std::string& str, const std::string& key) -> std::string {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\"'))
                    pos++;
                size_t end = pos;
                while (end < str.size()) {
                    if (str[end] == '\\' && end + 1 < str.size()) {
                        end += 2;
                        continue;
                    }
                    if (str[end] == '"')
                        break;
                    end++;
                }
                return str.substr(pos, end - pos);
            }
        }
        return "";
    };
    
    g_missionData.valid = true;
    g_missionData.status = parseJsonString(jsonData, "status");
    g_missionData.objectives.clear();
    
    // Парсим массив objectives
    size_t objectivesStart = jsonData.find("\"objectives\":[");
    if (objectivesStart != std::string::npos) {
        objectivesStart += 14; // Пропускаем "objectives":[
        size_t objectivesEnd = jsonData.find("]", objectivesStart);
        if (objectivesEnd != std::string::npos) {
            std::string objectivesArray = jsonData.substr(objectivesStart, objectivesEnd - objectivesStart);
            
            // Функция для поиска конца объекта
            auto findObjectEnd = [](const std::string& str, size_t start) -> size_t {
                int depth = 0;
                bool inString = false;
                for (size_t i = start; i < str.size(); i++) {
                    if (str[i] == '"' && (i == 0 || str[i-1] != '\\'))
                        inString = !inString;
                    if (!inString) {
                        if (str[i] == '{') depth++;
                        if (str[i] == '}') {
                            depth--;
                            if (depth == 0) return i + 1;
                        }
                    }
                }
                return std::string::npos;
            };
            
            size_t objPos = 0;
            while ((objPos = objectivesArray.find("{", objPos)) != std::string::npos) {
                size_t objEnd = findObjectEnd(objectivesArray, objPos);
                if (objEnd == std::string::npos) break;
                
                std::string objStr = objectivesArray.substr(objPos, objEnd - objPos);
                
                MissionObjective objective;
                objective.primary = parseJsonBool(objStr, "primary");
                objective.status = parseJsonString(objStr, "status");
                objective.text = parseJsonString(objStr, "text");
                
                g_missionData.objectives.push_back(objective);
                
                objPos = objEnd;
            }
        }
    }
}

// Функция для перезагрузки карты
static void ReloadMapTexture() {
    // Освобождаем старую текстуру
    if (g_backgroundTexture) {
        g_backgroundTexture->Release();
        g_backgroundTexture = nullptr;
    }
    
    // Загружаем новую карту
    std::string mapUrl = "http://localhost:8111/map.img";
    std::vector<unsigned char> imageData;
    
    if (DownloadDataFromURL(mapUrl, imageData)) {
        // Растягиваем до 2048x2048
        g_backgroundWidth = 2048;
        g_backgroundHeight = 2048;
        if (LoadTextureFromMemory(imageData.data(), imageData.size(), &g_backgroundTexture, g_backgroundWidth, g_backgroundHeight)) {
            // Текстура успешно загружена и растянута
        } else {
            g_backgroundTexture = nullptr;
        }
    } else {
        // Если загрузка не удалась, оставляем nullptr
        g_backgroundTexture = nullptr;
    }
}

// Парсинг данных map_info
void ParseMapInfo(const std::string& jsonData) {
    std::lock_guard<std::mutex> lock(g_mapInfoMutex);
    
    // Функция для парсинга массива float из JSON
    auto parseJsonFloatArray = [](const std::string& str, const std::string& key, float* outArray, int count) -> bool {
        size_t pos = str.find("\"" + key + "\"");
        if (pos == std::string::npos) return false;
        
        pos = str.find("[", pos);
        if (pos == std::string::npos) return false;
        
        pos++; // Пропускаем [
        size_t end = str.find("]", pos);
        if (end == std::string::npos) return false;
        
        std::string arrayStr = str.substr(pos, end - pos);
        
        // Парсим элементы массива
        size_t itemStart = 0;
        int index = 0;
        while (itemStart < arrayStr.length() && index < count) {
            // Пропускаем пробелы и кавычки
            while (itemStart < arrayStr.length() && (arrayStr[itemStart] == ' ' || arrayStr[itemStart] == '\t' || arrayStr[itemStart] == '"'))
                itemStart++;
            
            if (itemStart >= arrayStr.length()) break;
            
            // Ищем конец числа (запятая, закрывающая скобка или пробел)
            size_t itemEnd = itemStart;
            while (itemEnd < arrayStr.length()) {
                char c = arrayStr[itemEnd];
                if (c == ',' || c == ']' || c == ' ' || c == '\t' || c == '"')
                    break;
                itemEnd++;
            }
            
            if (itemEnd > itemStart) {
                std::string numStr = arrayStr.substr(itemStart, itemEnd - itemStart);
                outArray[index] = static_cast<float>(std::atof(numStr.c_str()));
                index++;
            }
            
            // Пропускаем запятую
            if (itemEnd < arrayStr.length() && arrayStr[itemEnd] == ',')
                itemEnd++;
            
            itemStart = itemEnd;
        }
        
        return index == count;
    };
    
    // Функция для парсинга int из JSON
    auto parseJsonInt = [](const std::string& str, const std::string& key) -> int {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                // Ищем конец числа
                size_t end = pos;
                while (end < str.size()) {
                    char c = str[end];
                    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        break;
                    end++;
                }
                if (end > pos) {
                    std::string numStr = str.substr(pos, end - pos);
                    return std::atoi(numStr.c_str());
                }
            }
        }
        return 0;
    };
    
    g_mapInfoData.valid = true;
    parseJsonFloatArray(jsonData, "grid_steps", g_mapInfoData.gridSteps, 2);
    parseJsonFloatArray(jsonData, "grid_zero", g_mapInfoData.gridZero, 2);
    parseJsonFloatArray(jsonData, "map_min", g_mapInfoData.mapMin, 2);
    parseJsonFloatArray(jsonData, "map_max", g_mapInfoData.mapMax, 2);
    g_mapInfoData.hudType = parseJsonInt(jsonData, "hud_type");
    int newMapGeneration = parseJsonInt(jsonData, "map_generation");
    
    // Проверяем, изменилась ли карта
    if (g_lastMapGeneration != -1 && g_lastMapGeneration != newMapGeneration) {
        // Карта сменилась - перезагружаем текстуру
        ReloadMapTexture();
    }
    
    g_mapInfoData.mapGeneration = newMapGeneration;
    g_lastMapGeneration = newMapGeneration;
}

// Парсинг объектов карты (map_obj.json)
void ParseMapObjects(const std::string& jsonData) {
    std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
    
    // Помечаем все объекты как "не обновлённые"
    for (auto& obj : g_mapObjects)
        obj.initialized = false;
    
    // Функция для поиска конца объекта (с учетом вложенных объектов и строк)
    auto findObjectEnd = [](const std::string& str, size_t start) -> size_t {
        int depth = 0;
        bool inString = false;
        for (size_t i = start; i < str.size(); i++) {
            if (str[i] == '"' && (i == 0 || str[i-1] != '\\'))
                inString = !inString;
            if (!inString) {
                if (str[i] == '{') depth++;
                if (str[i] == '}') {
                    depth--;
                    if (depth == 0) return i + 1;
                }
            }
        }
        return std::string::npos;
    };
    
    // Парсим объекты
    size_t pos = 0;
    while ((pos = jsonData.find("{", pos)) != std::string::npos) {
        size_t endPos = findObjectEnd(jsonData, pos);
        if (endPos == std::string::npos) break;
        
        std::string objStr = jsonData.substr(pos, endPos - pos);
        pos = endPos;
        
        // Извлекаем данные объекта
        auto getStringField = [&](const std::string& key) -> std::string {
            size_t keyPos = objStr.find("\"" + key + "\"");
            if (keyPos != std::string::npos) {
                keyPos = objStr.find(":", keyPos);
                if (keyPos != std::string::npos) {
                    keyPos++;
                    while (keyPos < objStr.size() && (objStr[keyPos] == ' ' || objStr[keyPos] == '\"'))
                        keyPos++;
                    size_t end = keyPos;
                    while (end < objStr.size() && objStr[end] != '\"' && objStr[end] != ',')
                        end++;
                    return objStr.substr(keyPos, end - keyPos);
                }
            }
            return "";
        };
        
        auto getFloatField = [&](const std::string& key) -> float {
            size_t keyPos = objStr.find("\"" + key + "\"");
            if (keyPos != std::string::npos) {
                keyPos = objStr.find(":", keyPos);
                if (keyPos != std::string::npos) {
                    keyPos++;
                    while (keyPos < objStr.size() && (objStr[keyPos] == ' ' || objStr[keyPos] == '\t'))
                        keyPos++;
                    // Ищем конец числа
                    size_t end = keyPos;
                    while (end < objStr.size()) {
                        char c = objStr[end];
                        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                            break;
                        end++;
                    }
                    if (end > keyPos) {
                        std::string numStr = objStr.substr(keyPos, end - keyPos);
                        return static_cast<float>(std::atof(numStr.c_str()));
                    }
                }
            }
            return 0.0f;
        };
        
        std::string type = getStringField("type");
        std::string icon = getStringField("icon");
        float newX = getFloatField("x");
        float newY = getFloatField("y");
        float newDx = getFloatField("dx");
        float newDy = getFloatField("dy");
        
        // Парсим цвет для идентификации
        std::string newColorHash;
        float newR = 1.0f, newG = 1.0f, newB = 1.0f;
        {
            size_t keyPos = objStr.find("\"color\"");
            if (keyPos != std::string::npos && keyPos + 7 < objStr.size()) {
                if (objStr[keyPos + 7] == '[') {
                    // Массив цветов
                    size_t arrayStart = keyPos + 7;
                    size_t arrayEnd = objStr.find("]", arrayStart);
                    if (arrayEnd != std::string::npos) {
                        newColorHash = objStr.substr(arrayStart, arrayEnd - arrayStart + 1);
                        // Парсим первый цвет для отображения
                        size_t firstHash = newColorHash.find("#");
                        if (firstHash != std::string::npos) {
                            std::string hexStr = newColorHash.substr(firstHash + 1, 6);
                            size_t validLen = 0;
                            unsigned int colorValue = std::stoul(hexStr, &validLen, 16);
                            if (validLen == 6) {
                                newR = ((colorValue >> 16) & 0xFF) / 255.0f;
                                newG = ((colorValue >> 8) & 0xFF) / 255.0f;
                                newB = (colorValue & 0xFF) / 255.0f;
                            }
                        }
                    }
                } else {
                    // Строка цвета
                    size_t hashPos = objStr.find("#", keyPos);
                    if (hashPos != std::string::npos) {
                        newColorHash = objStr.substr(hashPos + 1, 6);
                        size_t validLen = 0;
                        unsigned int colorValue = std::stoul(newColorHash, &validLen, 16);
                        if (validLen == 6) {
                            newR = ((colorValue >> 16) & 0xFF) / 255.0f;
                            newG = ((colorValue >> 8) & 0xFF) / 255.0f;
                            newB = (colorValue & 0xFF) / 255.0f;
                        }
                    }
                }
            }
        }
        
        float currentTime = ImGui::GetTime();
        const float maxPositionChange = 0.1f; // Максимально допустимое изменение координат (10% карты)
        
        // Ищем существующий объект по нескольким критериям:
        // 1. type + icon должны совпадать
        // 2. colorHash должен совпадать (если есть)
        // 3. Координаты должны измениться в допустимом диапазоне
        bool found = false;
        MapObject* bestMatch = nullptr;
        float bestMatchScore = 0.0f;
        
        for (auto& obj : g_mapObjects) {
            if (!obj.initialized) {
                // Проверяем type и icon
                if (obj.type != type || obj.icon != icon) {
                    continue;
                }
                
                // Проверяем цвет (если есть)
                bool colorMatch = true;
                if (!newColorHash.empty() && !obj.colorHash.empty()) {
                    if (newColorHash != obj.colorHash) {
                        continue; // Разные цвета - это другой объект
                    }
                }
                
                // Проверяем изменение координат
                float distX = newX - obj.x;
                float distY = newY - obj.y;
                float dist = sqrtf(distX * distX + distY * distY);
                
                // Если координаты изменились слишком сильно - это другой объект
                if (dist > maxPositionChange) {
                    continue; // Пропускаем - координаты изменились слишком сильно
                }
                
                // Вычисляем score (чем меньше расстояние, тем лучше)
                float score = 1.0f / (1.0f + dist * 10.0f);
                if (colorMatch && !newColorHash.empty() && !obj.colorHash.empty()) {
                    score *= 2.0f; // Бонус за совпадение цвета
                }
                
                // Выбираем лучший match
                if (score > bestMatchScore) {
                    bestMatchScore = score;
                    bestMatch = &obj;
                }
            }
        }
        
        if (bestMatch) {
            // Сохраняем последнюю позицию и направление перед обновлением
            bestMatch->lastX = bestMatch->x;
            bestMatch->lastY = bestMatch->y;
            bestMatch->lastDx = bestMatch->dx;
            bestMatch->lastDy = bestMatch->dy;
            bestMatch->lastUpdateTime = currentTime;
            
            // Обновляем текущие значения
            bestMatch->x = newX;
            bestMatch->y = newY;
            bestMatch->dx = newDx;
            bestMatch->dy = newDy;
            
            // Обновляем направление (sx, sy, ex, ey)
            bestMatch->sx = getFloatField("sx");
            bestMatch->sy = getFloatField("sy");
            bestMatch->ex = getFloatField("ex");
            bestMatch->ey = getFloatField("ey");
            
            // Обновляем цвет
            bestMatch->r = newR;
            bestMatch->g = newG;
            bestMatch->b = newB;
            bestMatch->colorHash = newColorHash;
            
            bestMatch->initialized = true;
            bestMatch->missedUpdates = 0;
            found = true;
        }
        
        if (!found) {
            // Создаём новый объект (не найден в предыдущем списке)
            MapObject obj;
            obj.type = type;
            obj.icon = icon;
            obj.x = newX;
            obj.y = newY;
            obj.dx = newDx;
            obj.dy = newDy;
            obj.sx = getFloatField("sx");
            obj.sy = getFloatField("sy");
            obj.ex = getFloatField("ex");
            obj.ey = getFloatField("ey");
            obj.isPlayer = (icon == "Player");
            obj.r = newR;
            obj.g = newG;
            obj.b = newB;
            obj.colorHash = newColorHash;
            obj.initialized = true;
            obj.lastUpdateTime = currentTime;
            
            g_mapObjects.push_back(obj);
        }
    }
    
    // Удаляем объекты, которые не были обновлены
    g_mapObjects.erase(
        std::remove_if(g_mapObjects.begin(), g_mapObjects.end(),
            [](const MapObject& obj) { return !obj.initialized; }),
        g_mapObjects.end()
    );
    
    // Пересчитываем индексы выбранных юнитов после удаления объектов
    // Удаляем невалидные индексы
    g_selectedUnits.erase(
        std::remove_if(g_selectedUnits.begin(), g_selectedUnits.end(),
            [](size_t idx) { return idx >= g_mapObjects.size(); }),
        g_selectedUnits.end()
    );
    
}

// Инициализация UI
void InitializeUI()
{
    // Загружаем текстуру подложки с URL и растягиваем до 2048x2048
    ReloadMapTexture();
    
    // Загружаем настройки (окно должно быть уже создано)
    LoadSettings();
}

// Отрисовка UI
void RenderUI()
{
    // Получаем размер окна
    RECT clientRect;
    GetClientRect(g_hWnd, &clientRect);
    float windowWidth = (float)(clientRect.right - clientRect.left);
    float windowHeight = (float)(clientRect.bottom - clientRect.top);
    
    // Главное окно на весь экран
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    
    ImGuiWindowFlags flags = 
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); // Убираем закругления главного окна
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    
    ImGui::Begin("##MainWindow", nullptr, flags);
    
    // === TITLE BAR ===
    const float titleBarHeight = 28.0f;
    const float buttonSize = 12.0f; // macOS стиль - маленькие круглые кнопки
    const float buttonSpacing = 6.0f;
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
    ImGui::BeginChild("##TitleBar", ImVec2(windowWidth, titleBarHeight), false, ImGuiWindowFlags_NoScrollbar);
    
    // Заголовок
    ImGui::SetCursorPos(ImVec2(12, 6));
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Advanced Map War Thunder");
    
    // Кнопки управления окном (справа, в стиле macOS)
    float buttonY = (titleBarHeight - buttonSize) * 0.5f; // Центрируем по вертикали
    float buttonsStartX = windowWidth - (buttonSize * 3 + buttonSpacing * 2) - 8.0f;
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, buttonSize * 0.5f); // Полностью круглые
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(buttonSpacing, 0));
    
    // Кнопка свернуть (зеленая, macOS стиль) - первая справа
    ImGui::SetCursorPos(ImVec2(buttonsStartX, buttonY));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.69f, 0.3f, 1.0f)); // Зеленый macOS
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.75f, 0.33f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.11f, 0.6f, 0.26f, 1.0f));
    
    if (ImGui::Button("##Minimize", ImVec2(buttonSize, buttonSize)))
    {
        ShowWindow(g_hWnd, SW_MINIMIZE);
    }
    // Иконка минимизации (─) - только при hover
    if (ImGui::IsItemHovered())
    {
        ImVec2 minPos = ImGui::GetItemRectMin();
        float iconW = buttonSize * 0.4f;
        float iconX = minPos.x + buttonSize * 0.5f - iconW * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(iconX, minPos.y + buttonSize * 0.5f),
            ImVec2(iconX + iconW, minPos.y + buttonSize * 0.5f),
            IM_COL32(0, 0, 0, 200), 1.0f
        );
    }
    ImGui::PopStyleColor(3);
    
    // Кнопка развернуть (желтая, macOS стиль) - на весь экран
    ImGui::SetCursorPos(ImVec2(buttonsStartX + buttonSize + buttonSpacing, buttonY));
    
    // Проверяем, развернуто ли окно
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(g_hWnd, &wp);
    bool isMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.76f, 0.06f, 1.0f)); // Желтый macOS
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.8f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.65f, 0.05f, 1.0f));
    
    if (ImGui::Button("##Maximize", ImVec2(buttonSize, buttonSize)))
    {
        ShowWindow(g_hWnd, isMaximized ? SW_RESTORE : SW_MAXIMIZE);
    }
    // Иконка развернуть (□ или ↖↘) - только при hover
    if (ImGui::IsItemHovered())
    {
        ImVec2 maxPos = ImGui::GetItemRectMin();
        float iconSize = buttonSize * 0.35f;
        float iconX = maxPos.x + buttonSize * 0.5f - iconSize * 0.5f;
        float iconY = maxPos.y + buttonSize * 0.5f - iconSize * 0.5f;
        
        if (isMaximized)
        {
            // Два квадрата для восстановления
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(iconX, iconY + 1.5f),
                ImVec2(iconX + iconSize, iconY + iconSize + 1.5f),
                IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f
            );
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(iconX + 1.5f, iconY),
                ImVec2(iconX + iconSize + 1.5f, iconY + iconSize),
                IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f
            );
        }
        else
        {
            // Один квадрат для развернуть
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(iconX, iconY),
                ImVec2(iconX + iconSize, iconY + iconSize),
                IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f
            );
        }
    }
    ImGui::PopStyleColor(3);
    
    // Кнопка закрыть (красная, macOS стиль) - последняя справа
    ImGui::SetCursorPos(ImVec2(buttonsStartX + (buttonSize + buttonSpacing) * 2, buttonY));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.26f, 0.21f, 1.0f)); // Красный macOS
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.22f, 0.18f, 1.0f));
    
    if (ImGui::Button("##Close", ImVec2(buttonSize, buttonSize)))
    {
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
    }
    // Иконка закрытия (✕) - только при hover
    if (ImGui::IsItemHovered())
    {
        ImVec2 closePos = ImGui::GetItemRectMin();
        float iconSize = buttonSize * 0.4f;
        float iconX = closePos.x + buttonSize * 0.5f - iconSize * 0.5f;
        float iconY = closePos.y + buttonSize * 0.5f - iconSize * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(iconX, iconY),
            ImVec2(iconX + iconSize, iconY + iconSize),
            IM_COL32(0, 0, 0, 200), 1.0f
        );
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(iconX + iconSize, iconY),
            ImVec2(iconX, iconY + iconSize),
            IM_COL32(0, 0, 0, 200), 1.0f
        );
    }
    ImGui::PopStyleColor(3);
    
    ImGui::PopStyleVar(2);
    
    ImGui::EndChild();
    ImGui::PopStyleColor(); // Закрывает ChildBg (строка 48)
    
    // === КОНТЕНТ ===
    const float contentHeight = windowHeight - titleBarHeight;
    const float contentWidth = windowWidth;
    // Пропорциональные размеры относительно ширины окна
    const float padding = windowWidth * 0.01f; // 1% от ширины окна
    const float sidebarWidth = windowWidth * 0.175f; // 17.5% от ширины окна (примерно 280px при 1600px)
    
    // Ширина Content 2 (изменяемая через разделитель)
    static float content2Width = sidebarWidth; // Начальная ширина
    const float minContent2Width = windowWidth * 0.1f; // Минимальная ширина 10%
    const float maxContent2Width = windowWidth * 0.5f; // Максимальная ширина 50%
    
    // Ограничиваем ширину при изменении размера окна
    if (content2Width > maxContent2Width) content2Width = maxContent2Width;
    if (content2Width < minContent2Width) content2Width = minContent2Width;
    
    ImGui::SetCursorPos(ImVec2(0, titleBarHeight));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::BeginChild("##Content", ImVec2(contentWidth, contentHeight), false, ImGuiWindowFlags_NoScrollbar);
    
    // === ПОДЛОЖКА (интерактивная карта с pan & zoom) ===
    ImVec2 contentPos = ImGui::GetWindowPos();
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    
    // Получаем deltaTime для плавной инерции
    float deltaTime = ImGui::GetIO().DeltaTime;
    if (deltaTime > 0.1f) deltaTime = 0.1f; // Ограничиваем максимальный deltaTime
    
    // Обработка клавиши D для переключения дебаг режима
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        // Клавиша D только что нажата (без повторения)
        g_debugMode = !g_debugMode;
    }
    
    // Обработка клавиши F для переключения режима слежения
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        g_followMode = !g_followMode;
        if (g_followMode) {
            // Сбрасываем корректировку зума при включении
            g_followZoomAdjust = 1.0f;
        }
    }
    
    // Обработка клавиши C для очистки всех выделений и меток
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        g_selectedUnits.clear();
        {
            std::lock_guard<std::mutex> lock(g_mapMarkersMutex);
            g_mapMarkers.clear();
        }
    }
    
    // Обработка зума колесиком мыши
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (g_followMode) {
                // В режиме слежения - корректируем множитель зума
                // Максимальное значение будет ограничено в логике слежения с учетом Content 1
                g_followZoomAdjust += wheel * 0.15f * g_followZoomAdjust;
                if (g_followZoomAdjust < 0.3f) g_followZoomAdjust = 0.3f;
                // Временное ограничение (будет пересчитано в логике слежения)
                if (g_followZoomAdjust > 3.0f) g_followZoomAdjust = 3.0f;
            } else {
                // Обычный режим - зум к позиции курсора
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 contentScreenPos = ImGui::GetWindowPos();
                
                // Вычисляем относительную позицию мыши на карте
                float mouseRelX = (mousePos.x - contentScreenPos.x - contentSize.x * 0.5f - g_targetOffsetX) / g_targetZoom;
                float mouseRelY = (mousePos.y - contentScreenPos.y - contentSize.y * 0.5f - g_targetOffsetY) / g_targetZoom;
                
                float oldZoom = g_targetZoom;
                g_targetZoom += wheel * 0.15f * g_targetZoom;
                g_targetZoom = (g_targetZoom < 0.1f) ? 0.1f : (g_targetZoom > 10.0f) ? 10.0f : g_targetZoom;
                
                // Корректировка offset чтобы зум был к позиции курсора
                g_targetOffsetX -= mouseRelX * (g_targetZoom - oldZoom);
                g_targetOffsetY -= mouseRelY * (g_targetZoom - oldZoom);
            }
        }
        
        // Перетаскивание ЛКМ для перемещения карты с инерцией
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            
            // Применяем перемещение сразу
            g_mapOffsetX += delta.x;
            g_mapOffsetY += delta.y;
            
            // Добавляем скорость для инерции (накапливаем скорость при перетаскивании)
            g_offsetXVelocity += delta.x * 0.5f;
            g_offsetYVelocity += delta.y * 0.5f;
            
            // Обновляем целевые значения
            g_targetOffsetX = g_mapOffsetX;
            g_targetOffsetY = g_mapOffsetY;
            
            // Отключаем режим слежения при ручном перемещении
            g_followMode = false;
            
            // Помечаем что был drag
            g_wasDragging = true;
            
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
        
        // Сброс зума и позиции по СКМ с инерцией (только для offset)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            // Вычисляем разницу для создания инерции offset
            float offsetXDiff = 0.0f - g_mapOffsetX;
            float offsetYDiff = 0.0f - g_mapOffsetY;
            
            // Добавляем скорость для плавного возврата offset
            g_offsetXVelocity = offsetXDiff * 0.3f;
            g_offsetYVelocity = offsetYDiff * 0.3f;
            
            // Устанавливаем целевые значения (зум сразу, offset через инерцию)
            g_targetZoom = 1.0f;
            g_mapZoom = 1.0f; // Зум сразу
            g_targetOffsetX = 0.0f;
            g_targetOffsetY = 0.0f;
            
            // Отключаем режим слежения при сбросе
            g_followMode = false;
        }
        
        // Сбрасываем флаг drag при отпускании мыши
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            g_wasDragging = false;
        }
    }
    
    // Плавная интерполяция зума (без инерции для зума, только для offset)
    float lerpSpeed = 0.15f;
    g_mapZoom += (g_targetZoom - g_mapZoom) * lerpSpeed;
    
    // Применяем инерцию к offset
    if (fabsf(g_offsetXVelocity) > g_minVelocity || fabsf(g_offsetYVelocity) > g_minVelocity) {
        g_mapOffsetX += g_offsetXVelocity * deltaTime * 60.0f;
        g_mapOffsetY += g_offsetYVelocity * deltaTime * 60.0f;
        
        g_offsetXVelocity *= g_friction;
        g_offsetYVelocity *= g_friction;
    } else {
        g_offsetXVelocity = 0.0f;
        g_offsetYVelocity = 0.0f;
    }
    
    // Логика слежения камеры (если включен режим слежения)
    if (g_followMode) {
        std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
        if (!g_mapObjects.empty()) {
            // Находим игрока
            const MapObject* playerUnit = nullptr;
            for (const auto& obj : g_mapObjects) {
                if (obj.isPlayer) {
                    playerUnit = &obj;
                    break;
                }
            }
            
            if (playerUnit) {
                // Собираем все точки для отслеживания
                float minX = playerUnit->x;
                float maxX = minX;
                float minY = playerUnit->y;
                float maxY = minY;
                
                // Добавляем выбранные юниты
                for (size_t selIdx : g_selectedUnits) {
                    if (selIdx < g_mapObjects.size()) {
                        float x = g_mapObjects[selIdx].x;
                        float y = g_mapObjects[selIdx].y;
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
                
                // Добавляем метки
                {
                    std::lock_guard<std::mutex> lock2(g_mapMarkersMutex);
                    for (const auto& marker : g_mapMarkers) {
                        if (marker.x < minX) minX = marker.x;
                        if (marker.x > maxX) maxX = marker.x;
                        if (marker.y < minY) minY = marker.y;
                        if (marker.y > maxY) maxY = marker.y;
                    }
                }
                
                // Центр всех точек
                float centerX = (minX + maxX) * 0.5f;
                float centerY = (minY + maxY) * 0.5f;
                
                // Размер области объектов в нормализованных координатах (с отступом 15%)
                float rangeX = (maxX - minX) * 1.3f;
                float rangeY = (maxY - minY) * 1.3f;
                
                // Минимальный размер области (если только игрок)
                if (rangeX < 0.05f) rangeX = 0.05f;
                if (rangeY < 0.05f) rangeY = 0.05f;
                
                // Получаем размер окна (область контента)
                ImVec2 contentSize = ImGui::GetContentRegionAvail();
                
                // Учитываем ширину Content 1 (левая панель) + дополнительный отступ
                // Вычисляем ширину sidebar на основе размера окна (как в основном коде)
                ImVec2 windowSize = ImGui::GetWindowSize();
                float sidebarWidth = windowSize.x * 0.175f; // 17.5% от ширины окна
                if (sidebarWidth < 200.0f) sidebarWidth = 200.0f;
                if (sidebarWidth > 400.0f) sidebarWidth = 400.0f;
                
                // Добавляем 1/4 от размера Content 1 как дополнительную преграду
                float content1Obstacle = sidebarWidth * 1.445; // sidebarWidth + sidebarWidth * 0.25
                
                // Доступная ширина = общая ширина - ширина Content 1 - дополнительный отступ
                float windowWidth = contentSize.x - content1Obstacle;
                float windowHeight = contentSize.y;
                
                // Защита от отрицательных значений
                if (windowWidth < 100.0f) windowWidth = 100.0f;
                
                // Базовый размер карты
                float baseImageSize = 2048.0f;
                
                // Рассчитываем максимальный зум для каждой оси
                // rangeX * baseImageSize * zoom <= windowWidth
                // zoom <= windowWidth / (rangeX * baseImageSize)
                float maxZoomX = windowWidth / (rangeX * baseImageSize);
                float maxZoomY = windowHeight / (rangeY * baseImageSize);
                
                // Берем минимальный зум, чтобы все поместилось по обеим осям
                float neededZoom = (maxZoomX < maxZoomY) ? maxZoomX : maxZoomY;
                
                // Максимальный зум с учетом Content 1 (чтобы интерфейс не перекрывал объекты)
                float maxAllowedZoom = neededZoom;
                
                // Ограничиваем g_followZoomAdjust, чтобы итоговый зум не превышал maxAllowedZoom
                // neededZoom * g_followZoomAdjust <= maxAllowedZoom
                // g_followZoomAdjust <= maxAllowedZoom / neededZoom
                float maxAllowedAdjust = maxAllowedZoom / neededZoom;
                if (g_followZoomAdjust > maxAllowedAdjust) {
                    g_followZoomAdjust = maxAllowedAdjust;
                }
                
                // Применяем корректировку зума от пользователя
                neededZoom *= g_followZoomAdjust;
                
                // Ограничиваем зум: минимальный и максимальный (не больше чем позволяет Content 1)
                if (neededZoom < 0.3f) neededZoom = 0.3f;
                if (neededZoom > maxAllowedZoom) neededZoom = maxAllowedZoom;
                
                // Плавно меняем зум
                g_targetZoom = neededZoom;
                
                // Рассчитываем смещение чтобы центрировать
                float targetCenterX = centerX - 0.5f;
                float targetCenterY = centerY - 0.5f;
                
                // Пересчитываем imgDisplaySize с новым зумом
                float newImgDisplaySize = baseImageSize * g_targetZoom;
                
                g_targetOffsetX = -targetCenterX * newImgDisplaySize;
                g_targetOffsetY = -targetCenterY * newImgDisplaySize;
            }
        }
    }
    
    // Плавная интерполяция к целевым значениям offset (если есть разница)
    if (fabsf(g_targetOffsetX - g_mapOffsetX) > 0.1f) {
        g_mapOffsetX += (g_targetOffsetX - g_mapOffsetX) * lerpSpeed;
    }
    if (fabsf(g_targetOffsetY - g_mapOffsetY) > 0.1f) {
        g_mapOffsetY += (g_targetOffsetY - g_mapOffsetY) * lerpSpeed;
    }
    
    // Отображаем карту с учетом зума и offset
    if (g_backgroundTexture) {
        float baseImageSize = 2048.0f; // Базовый размер карты
        float imgDisplaySize = baseImageSize * g_mapZoom;
        
        float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
        float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddImage(
            (ImTextureID)g_backgroundTexture,
            ImVec2(contentPos.x + imgX, contentPos.y + imgY),
            ImVec2(contentPos.x + imgX + imgDisplaySize, contentPos.y + imgY + imgDisplaySize),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
        
        // Отрисовка грид-сетки для отладки
        {
            std::lock_guard<std::mutex> lock(g_mapInfoMutex);
            if (g_mapInfoData.valid) {
                // === Расчёт сетки для картинки 2048x2048 ===
                const float originalImageSize = 2048.0f;
                
                // Размер карты в игровых единицах
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                
                // Пиксели на игровую единицу (для оригинальной картинки 2048x2048)
                float origPixelsPerUnitX = originalImageSize / mapSizeX;
                float origPixelsPerUnitY = originalImageSize / mapSizeY;
                
                // Шаг сетки в пикселях оригинальной картинки
                float origGridStepX = g_mapInfoData.gridSteps[0] * origPixelsPerUnitX;
                float origGridStepY = g_mapInfoData.gridSteps[1] * origPixelsPerUnitY;
                
                // Позиция grid_zero в пикселях оригинальной картинки
                float origGridZeroX = (g_mapInfoData.gridZero[0] - g_mapInfoData.mapMin[0]) * origPixelsPerUnitX;
                float origGridZeroY = (g_mapInfoData.mapMax[1] - g_mapInfoData.gridZero[1]) * origPixelsPerUnitY;
                
                // === Масштабирование к текущему отображению ===
                float displayScale = imgDisplaySize / originalImageSize;
                
                float gridStepPxX = origGridStepX * displayScale;
                float gridStepPxY = origGridStepY * displayScale;
                float gridZeroPxX = origGridZeroX * displayScale;
                float gridZeroPxY = origGridZeroY * displayScale;
                
                // Цвет сетки
                ImU32 gridColor = IM_COL32(255, 255, 255, 50);
                
                // Clipping rect для сетки (только внутри карты)
                drawList->PushClipRect(
                    ImVec2(contentPos.x + imgX, contentPos.y + imgY),
                    ImVec2(contentPos.x + imgX + imgDisplaySize, contentPos.y + imgY + imgDisplaySize),
                    true
                );
                
                // Цвет текста индексов
                ImU32 textColor = IM_COL32(255, 255, 255, 180);
                
                // Масштабируем размер шрифта в зависимости от зума
                ImGuiStyle& style = ImGui::GetStyle();
                float baseFontSize = style.FontSizeBase;
                float scaledFontSize = baseFontSize * g_mapZoom;
                ImGui::PushFont(nullptr, scaledFontSize);
                
                float firstLineX, firstLineY;
                int startIndexX, startIndexY;
                
                if (g_mapInfoData.hudType == 0) {
                    // hud_type = 0 (авиа): сетка от левого верхнего угла
                    firstLineX = contentPos.x + imgX;
                    firstLineY = contentPos.y + imgY;
                    startIndexX = 1;
                    startIndexY = 0;
                } else {
                    // hud_type = 1 (танки): сетка от grid_zero
                    // Вычисляем сколько ячеек от левого края картинки до grid_zero
                    float cellsFromLeftToZero = gridZeroPxX / gridStepPxX;
                    float cellsFromTopToZero = gridZeroPxY / gridStepPxY;
                    
                    // Индекс первой видимой ячейки (левый край картинки)
                    int indexAtLeftEdge = 1 - (int)std::floor(cellsFromLeftToZero);
                    int indexAtTopEdge = -(int)std::floor(cellsFromTopToZero); // Буквы с 0 (A)
                    
                    // Находим первую линию
                    float offsetX = fmodf(gridZeroPxX, gridStepPxX);
                    float offsetY = fmodf(gridZeroPxY, gridStepPxY);
                    
                    firstLineX = contentPos.x + imgX + offsetX;
                    firstLineY = contentPos.y + imgY + offsetY;
                    
                    // Вычисляем индекс первой линии
                    startIndexX = indexAtLeftEdge;
                    if (offsetX > 0.001f) startIndexX++; // Первая линия правее левого края
                    
                    startIndexY = indexAtTopEdge;
                    if (offsetY > 0.001f) startIndexY++; // Первая линия ниже верхнего края
                }
                
                // Вертикальные линии — индексы цифрами сверху
                int indexX = startIndexX;
                for (float x = firstLineX; x <= contentPos.x + imgX + imgDisplaySize; x += gridStepPxX) {
                    drawList->AddLine(
                        ImVec2(x, contentPos.y + imgY),
                        ImVec2(x, contentPos.y + imgY + imgDisplaySize),
                        gridColor,
                        1.0f
                    );
                    
                    // Индекс цифрой (сверху по центру ячейки)
                    if (x + gridStepPxX * 0.5f <= contentPos.x + imgX + imgDisplaySize && indexX > 0) {
                        char label[8];
                        snprintf(label, sizeof(label), "%d", indexX);
                        ImVec2 textSize = ImGui::CalcTextSize(label);
                        float labelX = x + gridStepPxX * 0.5f - textSize.x * 0.5f;
                        float labelY = contentPos.y + imgY + 4.0f;
                        if (labelX >= contentPos.x + imgX && labelX + textSize.x <= contentPos.x + imgX + imgDisplaySize) {
                            drawList->AddText(ImVec2(labelX, labelY), textColor, label);
                        }
                    }
                    indexX++;
                }
                
                // Горизонтальные линии — индексы буквами слева
                int indexY = startIndexY;
                for (float y = firstLineY; y <= contentPos.y + imgY + imgDisplaySize; y += gridStepPxY) {
                    drawList->AddLine(
                        ImVec2(contentPos.x + imgX, y),
                        ImVec2(contentPos.x + imgX + imgDisplaySize, y),
                        gridColor,
                        1.0f
                    );
                    
                    // Индекс буквой (слева по центру ячейки)
                    if (y + gridStepPxY * 0.5f <= contentPos.y + imgY + imgDisplaySize && indexY >= 0) {
                        char label[8];
                        int letterIndex = indexY % 26;
                        label[0] = 'A' + letterIndex;
                        label[1] = '\0';
                        // Если больше 26 — добавляем вторую букву (AA, AB...)
                        if (indexY >= 26) {
                            label[0] = 'A' + (indexY / 26) - 1;
                            label[1] = 'A' + letterIndex;
                            label[2] = '\0';
                        }
                        
                        ImVec2 textSize = ImGui::CalcTextSize(label);
                        float labelPosX = contentPos.x + imgX + 4.0f;
                        float labelPosY = y + gridStepPxY * 0.5f - textSize.y * 0.5f;
                        if (labelPosY >= contentPos.y + imgY && labelPosY + textSize.y <= contentPos.y + imgY + imgDisplaySize) {
                            drawList->AddText(ImVec2(labelPosX, labelPosY), textColor, label);
                        }
                    }
                    indexY++;
                }
                
                // Восстанавливаем размер шрифта
                ImGui::PopFont();
                
                drawList->PopClipRect();
            }
        }
    }
    
    // === ОТРИСОВКА МЕТОК НА КАРТЕ ===
    {
        std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
        
        if (g_backgroundTexture && !g_mapObjects.empty()) {
            float baseImageSize = 2048.0f;
            float imgDisplaySize = baseImageSize * g_mapZoom;
            float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
            float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
            
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            
            // Функция для получения глифа по иконке (из старого проекта + браузер)
            // В браузере используется шрифт Icons с символами '4', '5', '6', '7', '8', '9', '0', '.', ':'
            // Для остальных используется первый символ иконки: item['icon'][0]
            // У нас используется symbols_skyquake.ttf с Unicode символами
            auto getIconGlyph = [](const std::string& icon) -> const char* {
                static char iconGlyphBuffer[8] = {0}; // Буфер для первого символа иконки
                // Специальные иконки (как в браузере)
                if (icon == "Airdefence") return "\xE2\x94\xB0";        // U+2530 - SPAA (символ '4' в браузере)
                if (icon == "Structure") return "\xE2\x94\xB4";         // U+2534 - SPG (символ '5' в браузере)
                if (icon == "waypoint") return "\xE2\x94\x98";          // U+2518 - Ship (символ '6' в браузере)
                if (icon == "capture_zone") return "\xE2\x95\xB8";      // U+2578 - Rhombus (символ '7' в браузере)
                if (icon == "bombing_point") return "\xE2\x96\xB5";     // U+25B5 - маркер (символ '8' в браузере)
                if (icon == "defending_point") return "\xE2\x95\xBD";   // U+257D - Circle (символ '9' в браузере)
                if (icon == "respawn_base_tank") return "\xE2\x94\xAC"; // U+252C - Medium Tank (символ '0' в браузере)
                if (icon == "respawn_base_fighter") return "\xE2\x96\xAD"; // U+25AD (символ '.' в браузере)
                if (icon == "respawn_base_bomber") return "\xE2\x96\xAD";  // U+25AD (символ ':' в браузере)
                
                // Авиация (из старого проекта)
                if (icon == "Fighter") return "\xE2\x94\xA4";          // U+2524 - Fighter
                if (icon == "Assault") return "\xE2\x94\x9E";          // U+251E - Attacker
                if (icon == "Bomber") return "\xE2\x94\xA0";           // U+2520 - Bomber
                if (icon == "Interceptor") return "\xE2\x94\xA4";      // U+2524 - Fighter
                
                // Наземная техника
                if (icon == "HeavyTank") return "\xE2\x94\xA8";        // U+2528 - Heavy Tank
                if (icon == "MediumTank") return "\xE2\x94\xAC";       // U+252C - Medium Tank
                if (icon == "LightTank") return "\xE2\x94\xAA";        // U+252A - Light Tank
                if (icon == "SPAA") return "\xE2\x94\xB0";             // U+2530 - SPAA
                if (icon == "SPG") return "\xE2\x94\xB4";              // U+2534 - SPG
                if (icon == "TankDestroyer") return "\xE2\x94\xB4";    // U+2534 - SPG/Tank Destroyer
                
                // Флот
                if (icon == "Ship") return "\xE2\x94\x98";             // U+2518 - Ship 1
                if (icon == "Boat") return "\xE2\x94\xAE";             // U+252E - Ship 2
                if (icon == "TorpedoBoat") return "\xE2\x94\xAE";      // U+252E - Ship 2
                if (icon == "Destroyer") return "\xE2\x94\x98";        // U+2518 - Ship 1
                if (icon == "Cruiser") return "\xE2\x94\x98";          // U+2518 - Ship 1
                
                // Player использует ту же иконку что и тип техники
                if (icon == "Player") return "\xE2\x94\xAC";           // U+252C - Medium Tank (default player)
                
                // Точки захвата A, B, C
                if (icon == "capture_zone_a" || icon == "A") return "\xE2\x95\xB8";  // U+2578 - Rhombus A
                if (icon == "capture_zone_b" || icon == "B") return "\xE2\x95\xBD";  // U+257D - Circle B
                if (icon == "capture_zone_c" || icon == "C") return "\xE2\x95\xBA";  // U+257A - Rhombus C
                
                // point_of_interest
                if (icon == "point_of_interest") return "\xE2\x95\xA8"; // U+2568
                
                // Tracked, Wheeled
                if (icon == "Tracked") return "\xE2\x94\xA8";           // U+2528 - Heavy Tank
                if (icon == "Wheeled") return "\xE2\x94\xAA";            // U+252A - Light Tank
                
                // Fallback: используем первый символ иконки (как в браузере)
                if (!icon.empty()) {
                    iconGlyphBuffer[0] = icon[0];
                    iconGlyphBuffer[1] = '\0';
                    return iconGlyphBuffer;
                }
                
                return "\xE2\x97\xA3";  // U+25E3 - default для неизвестных иконок
            };
            
            // Внутренний символ для bombing_point
            auto getBombingPointInner = []() -> const char* {
                return "\xE2\x96\x91";  // U+2591 - внутренний символ
            };
            
            // Размер шрифта иконок (масштабируется с зумом)
            float iconFontSize = 12.0f * g_mapZoom;
            if (iconFontSize < 7.0f) iconFontSize = 7.0f;
            if (iconFontSize > 21.0f) iconFontSize = 21.0f;
            
            // Clipping rect для меток (только внутри карты)
            drawList->PushClipRect(
                ImVec2(contentPos.x + imgX, contentPos.y + imgY),
                ImVec2(contentPos.x + imgX + imgDisplaySize, contentPos.y + imgY + imgDisplaySize),
                true
            );
            
            // Используем шрифт иконок, если он загружен
            ImFont* iconFont = g_customFont;
            
            // Проверяем, какие юниты выбраны
            std::vector<bool> isSelected(g_mapObjects.size(), false);
            for (size_t selIdx : g_selectedUnits) {
                if (selIdx < g_mapObjects.size()) {
                    isSelected[selIdx] = true;
                }
            }
            
            for (size_t i = 0; i < g_mapObjects.size(); i++) {
                const auto& obj = g_mapObjects[i];
                // Пропускаем объекты без координат
                if (obj.x == 0.0f && obj.y == 0.0f && obj.type != "airfield") continue;
                
                // В дебаг режиме рисуем радиус клика для юнитов
                if (g_debugMode && obj.type != "airfield" && !obj.isPlayer) {
                    float objScreenX = contentPos.x + imgX + obj.x * imgDisplaySize;
                    float objScreenY = contentPos.y + imgY + obj.y * imgDisplaySize;
                    float clickRadius = 0.002f; // Радиус в нормализованных координатах
                    float screenRadius = clickRadius * imgDisplaySize;
                    drawList->AddCircle(
                        ImVec2(objScreenX, objScreenY),
                        screenRadius,
                        IM_COL32(255, 255, 0, 150),
                        0,
                        1.5f
                    );
                }
                
                // В дебаг режиме рисуем направление движения и предсказанную траекторию
                if (g_debugMode && obj.type != "airfield" && !obj.isPlayer) {
                    // Проверяем, есть ли направление в sx/sy/ex/ey (начало и конец вектора направления)
                    bool hasDirection = (obj.sx != 0.0f || obj.sy != 0.0f || obj.ex != 0.0f || obj.ey != 0.0f) &&
                                        (fabsf(obj.ex - obj.sx) > 0.0001f || fabsf(obj.ey - obj.sy) > 0.0001f);
                    
                    if (hasDirection) {
                        float objScreenX = contentPos.x + imgX + obj.x * imgDisplaySize;
                        float objScreenY = contentPos.y + imgY + obj.y * imgDisplaySize;
                        
                        // Вычисляем направление из sx/sy -> ex/ey
                        float dirX = obj.ex - obj.sx;
                        float dirY = obj.ey - obj.sy;
                        float dirLen = sqrtf(dirX * dirX + dirY * dirY);
                        
                        if (dirLen > 0.0001f) {
                            // Нормализуем направление
                            dirX /= dirLen;
                            dirY /= dirLen;
                            
                            // Рисуем направление движения (стрелка)
                            float arrowLength = 20.0f * g_mapZoom;
                            if (arrowLength < 10.0f) arrowLength = 10.0f;
                            if (arrowLength > 30.0f) arrowLength = 30.0f;
                            
                            // Инвертируем Y для экранных координат
                            float screenDirX = dirX;
                            float screenDirY = -dirY;
                            
                            float endX = objScreenX + screenDirX * arrowLength;
                            float endY = objScreenY + screenDirY * arrowLength;
                            
                            // Рисуем линию направления (зеленая)
                            drawList->AddLine(
                                ImVec2(objScreenX, objScreenY),
                                ImVec2(endX, endY),
                                IM_COL32(0, 255, 0, 200),
                                2.0f
                            );
                            
                            // Рисуем стрелку на конце
                            float arrowSize = 5.0f;
                            float arrowAngle = atan2f(screenDirY, screenDirX);
                            float arrowAngle1 = arrowAngle + 2.5f;
                            float arrowAngle2 = arrowAngle - 2.5f;
                            ImVec2 arrow1(endX - cosf(arrowAngle1) * arrowSize, endY - sinf(arrowAngle1) * arrowSize);
                            ImVec2 arrow2(endX - cosf(arrowAngle2) * arrowSize, endY - sinf(arrowAngle2) * arrowSize);
                            drawList->AddTriangleFilled(
                                ImVec2(endX, endY),
                                arrow1,
                                arrow2,
                                IM_COL32(0, 255, 0, 200)
                            );
                            
                            // Рисуем предсказанную траекторию на 3 шага вперед
                            // Используем направление из sx/sy -> ex/ey и масштабируем
                            float stepSize = 0.01f; // Размер шага в нормализованных координатах (уменьшено в 10 раз)
                            
                            ImVec2 prevPoint(objScreenX, objScreenY);
                            float currentX = obj.x;
                            float currentY = obj.y;
                            
                            for (int step = 1; step <= 3; step++) {
                                // Предсказываем позицию на шаг вперед по направлению
                                float predictedX = currentX + dirX * stepSize * step;
                                float predictedY = currentY + dirY * stepSize * step;
                                
                                float predScreenX = contentPos.x + imgX + predictedX * imgDisplaySize;
                                float predScreenY = contentPos.y + imgY + predictedY * imgDisplaySize;
                                
                                // Рисуем линию к предсказанной точке (голубая)
                                drawList->AddLine(
                                    prevPoint,
                                    ImVec2(predScreenX, predScreenY),
                                    IM_COL32(100, 200, 255, 150),
                                    1.5f
                                );
                                
                                // Рисуем точку на предсказанной позиции
                                drawList->AddCircleFilled(
                                    ImVec2(predScreenX, predScreenY),
                                    3.0f,
                                    IM_COL32(100, 200, 255, 200),
                                    0
                                );
                                
                                prevPoint = ImVec2(predScreenX, predScreenY);
                            }
                        }
                    }
                }
                
                ImU32 objColor = IM_COL32(
                    (int)(obj.r * 255.0f),
                    (int)(obj.g * 255.0f),
                    (int)(obj.b * 255.0f),
                    255
                );
                
                // Аэродром - рисуем линию (толщина масштабируется с зумом, в 2 раза толще чем в браузере)
                if (obj.type == "airfield") {
                    float startX = contentPos.x + imgX + obj.sx * imgDisplaySize;
                    float startY = contentPos.y + imgY + obj.sy * imgDisplaySize;
                    float endX = contentPos.x + imgX + obj.ex * imgDisplaySize;
                    float endY = contentPos.y + imgY + obj.ey * imgDisplaySize;
                    float lineWidth = 5.0f * sqrtf(g_mapZoom); // В 2 раза толще чем в браузере (было 3.0 * sqrt(map_scale))
                    drawList->AddLine(ImVec2(startX, startY), ImVec2(endX, endY), objColor, lineWidth);
                    continue;
                }
                
                // Вычисляем позицию на экране (нормализованные координаты 0-1)
                float objScreenX = contentPos.x + imgX + obj.x * imgDisplaySize;
                float objScreenY = contentPos.y + imgY + obj.y * imgDisplaySize;
                
                // Визуальное выделение выбранных юнитов
                bool selected = isSelected[i];
                if (selected) {
                    float selectionRadius = 15.0f * g_mapZoom;
                    if (selectionRadius < 10.0f) selectionRadius = 10.0f;
                    if (selectionRadius > 25.0f) selectionRadius = 25.0f;
                    drawList->AddCircle(
                        ImVec2(objScreenX, objScreenY),
                        selectionRadius,
                        IM_COL32(100, 255, 100, 255),
                        0,
                        2.0f
                    );
                }
                
                // Получаем глиф для иконки
                const char* glyph = getIconGlyph(obj.icon);
                
                // Player рисуем треугольником с направлением
                if (obj.isPlayer) {
                    float objSize = 10.0f * g_mapZoom;
                    if (objSize < 6.0f) objSize = 6.0f;
                    if (objSize > 16.0f) objSize = 16.0f;
                    
                    float angle = atan2f(-obj.dy, obj.dx);
                    ImVec2 p1(objScreenX + cosf(angle) * objSize, objScreenY - sinf(angle) * objSize);
                    ImVec2 p2(objScreenX + cosf(angle + 2.4f) * objSize * 0.6f, objScreenY - sinf(angle + 2.4f) * objSize * 0.6f);
                    ImVec2 p3(objScreenX + cosf(angle - 2.4f) * objSize * 0.6f, objScreenY - sinf(angle - 2.4f) * objSize * 0.6f);
                    
                    // Белая обводка
                    drawList->AddTriangle(p1, p2, p3, IM_COL32(255, 255, 255, 255), 2.0f);
                    // Заливка цветом
                    drawList->AddTriangleFilled(p1, p2, p3, objColor);
                }
                else if (iconFont) {
                    // Рисуем иконку шрифтом
                    ImVec2 textSize = iconFont->CalcTextSizeA(iconFontSize, FLT_MAX, 0.0f, glyph);
                    float textX = objScreenX - textSize.x * 0.5f;
                    float textY = objScreenY - textSize.y * 0.5f;
                    
                    // Для bombing_point рисуем только внутренний символ жирным
                    if (obj.icon == "bombing_point") {
                        const char* innerGlyph = getBombingPointInner();
                        ImVec2 innerTextSize = iconFont->CalcTextSizeA(iconFontSize, FLT_MAX, 0.0f, innerGlyph);
                        float innerX = objScreenX - innerTextSize.x * 0.5f;
                        float innerY = objScreenY - innerTextSize.y * 0.5f;
                        
                        // Чёрная обводка (жирная - рисуем несколько раз со смещением)
                        for (int dx = -1; dx <= 1; dx++) {
                            for (int dy = -1; dy <= 1; dy++) {
                                drawList->AddText(iconFont, iconFontSize, ImVec2(innerX + dx, innerY + dy), IM_COL32(0, 0, 0, 200), innerGlyph);
                            }
                        }
                        
                        // Жирный символ - рисуем несколько раз со смещением для эффекта bold
                        drawList->AddText(iconFont, iconFontSize, ImVec2(innerX, innerY), objColor, innerGlyph);
                        drawList->AddText(iconFont, iconFontSize, ImVec2(innerX + 0.5f, innerY), objColor, innerGlyph);
                        drawList->AddText(iconFont, iconFontSize, ImVec2(innerX + 1.0f, innerY), objColor, innerGlyph);
                    }
                    // Для point_of_interest - жирный, увеличенный в 1.75 раза, розовый цвет
                    else if (obj.icon == "point_of_interest") {
                        float poiSize = iconFontSize * 1.75f;
                        ImVec2 poiTextSize = iconFont->CalcTextSizeA(poiSize, FLT_MAX, 0.0f, glyph);
                        float poiX = objScreenX - poiTextSize.x * 0.5f;
                        float poiY = objScreenY - poiTextSize.y * 0.5f;
                        
                        ImU32 pinkColor = IM_COL32(255, 105, 180, 255);  // Розовый цвет
                        
                        // Чёрная обводка (жирная)
                        for (int dx = -1; dx <= 1; dx++) {
                            for (int dy = -1; dy <= 1; dy++) {
                                drawList->AddText(iconFont, poiSize, ImVec2(poiX + dx, poiY + dy), IM_COL32(0, 0, 0, 200), glyph);
                            }
                        }
                        
                        // Жирный символ розовым цветом
                        drawList->AddText(iconFont, poiSize, ImVec2(poiX, poiY), pinkColor, glyph);
                        drawList->AddText(iconFont, poiSize, ImVec2(poiX + 0.5f, poiY), pinkColor, glyph);
                        drawList->AddText(iconFont, poiSize, ImVec2(poiX + 1.0f, poiY), pinkColor, glyph);
                    }
                    else {
                        // Для respawn_base_fighter и respawn_base_bomber - поворот по направлению (как в браузере)
                        bool rotate = (obj.type == "respawn_base_fighter") || (obj.type == "respawn_base_bomber");
                        
                        if (rotate && (obj.dx != 0.0f || obj.dy != 0.0f)) {
                            // В браузере используется ctx.rotate, но в ImGui нет прямого поворота текста
                            // Используем упрощенный вариант - рисуем без поворота, но с учетом направления
                            // Для respawn_base обычно dx/dy указывают направление взлета
                            drawList->AddText(iconFont, iconFontSize, ImVec2(textX, textY), IM_COL32(0, 0, 0, 200), glyph);
                            drawList->AddText(iconFont, iconFontSize * 0.85f, ImVec2(textX, textY), objColor, glyph);
                        }
                        else {
                            // Чёрная обводка
                            drawList->AddText(iconFont, iconFontSize, ImVec2(textX, textY), IM_COL32(0, 0, 0, 200), glyph);
                            // Основная иконка
                            drawList->AddText(iconFont, iconFontSize * 0.85f, ImVec2(textX, textY), objColor, glyph);
                        }
                    }
                }
                else {
                    // Fallback — простые фигуры (если шрифт не загружен)
                    float objSize = 6.0f;
                    
                    if (obj.type == "aircraft") {
                        objSize = 8.0f;
                        float angle = atan2f(-obj.dy, obj.dx);
                        ImVec2 p1(objScreenX + cosf(angle) * objSize, objScreenY - sinf(angle) * objSize);
                        ImVec2 p2(objScreenX + cosf(angle + 2.4f) * objSize * 0.6f, objScreenY - sinf(angle + 2.4f) * objSize * 0.6f);
                        ImVec2 p3(objScreenX + cosf(angle - 2.4f) * objSize * 0.6f, objScreenY - sinf(angle - 2.4f) * objSize * 0.6f);
                        drawList->AddTriangleFilled(p1, p2, p3, objColor);
                    }
                    else if (obj.type == "ground_model") {
                        objSize = 5.0f;
                        drawList->AddRectFilled(
                            ImVec2(objScreenX - objSize, objScreenY - objSize),
                            ImVec2(objScreenX + objSize, objScreenY + objSize),
                            objColor
                        );
                    }
                    else if (obj.type == "bombing_point") {
                        objSize = 8.0f;
                        drawList->AddCircle(ImVec2(objScreenX, objScreenY), objSize, objColor, 0, 2.0f);
                        drawList->AddLine(ImVec2(objScreenX - objSize, objScreenY), ImVec2(objScreenX + objSize, objScreenY), objColor, 1.5f);
                        drawList->AddLine(ImVec2(objScreenX, objScreenY - objSize), ImVec2(objScreenX, objScreenY + objSize), objColor, 1.5f);
                    }
                    else {
                        drawList->AddCircleFilled(ImVec2(objScreenX, objScreenY), objSize, objColor);
                    }
                }
            }
            
            drawList->PopClipRect();
        }
    }
    
    // === ОТРИСОВКА МЕТОК ===
    {
        float baseImageSize = 2048.0f;
        float imgDisplaySize = baseImageSize * g_mapZoom;
        float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
        float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 mousePos = ImGui::GetMousePos();
        
        std::lock_guard<std::mutex> lock(g_mapMarkersMutex);
        
        // Отрисовываем метки (в обратном порядке для правильного hover)
        for (int mi = (int)g_mapMarkers.size() - 1; mi >= 0; mi--) {
            const auto& marker = g_mapMarkers[mi];
            float markerScreenX = contentPos.x + imgX + marker.x * imgDisplaySize;
            float markerScreenY = contentPos.y + imgY + marker.y * imgDisplaySize;
            
            ImU32 markerColor = IM_COL32(marker.r, marker.g, marker.b, 255);
            
            // Рисуем метку (ромб)
            float size = 8.0f;
            drawList->AddQuadFilled(
                ImVec2(markerScreenX, markerScreenY - size),
                ImVec2(markerScreenX + size, markerScreenY),
                ImVec2(markerScreenX, markerScreenY + size),
                ImVec2(markerScreenX - size, markerScreenY),
                markerColor
            );
            drawList->AddQuad(
                ImVec2(markerScreenX, markerScreenY - size),
                ImVec2(markerScreenX + size, markerScreenY),
                ImVec2(markerScreenX, markerScreenY + size),
                ImVec2(markerScreenX - size, markerScreenY),
                IM_COL32(0, 0, 0, 255),
                2.0f
            );
            
            // Номер метки
            char markerNum[8];
            snprintf(markerNum, sizeof(markerNum), "%d", (int)(mi + 1));
            ImVec2 numSize = ImGui::CalcTextSize(markerNum);
            drawList->AddText(
                ImVec2(markerScreenX - numSize.x * 0.5f, markerScreenY - numSize.y * 0.5f),
                IM_COL32(0, 0, 0, 255),
                markerNum
            );
            
            // Проверка наведения на метку для удаления через Ctrl+клик
            float dx = mousePos.x - markerScreenX;
            float dy = mousePos.y - markerScreenY;
            bool markerHovered = (dx * dx + dy * dy < size * size * 2);
            
            if (markerHovered) {
                // Подсветка при наведении
                drawList->AddQuad(
                    ImVec2(markerScreenX, markerScreenY - size - 2),
                    ImVec2(markerScreenX + size + 2, markerScreenY),
                    ImVec2(markerScreenX, markerScreenY + size + 2),
                    ImVec2(markerScreenX - size - 2, markerScreenY),
                    IM_COL32(255, 255, 255, 200),
                    2.0f
                );
                
                char tooltip[64];
                snprintf(tooltip, sizeof(tooltip), TR().Get("marker_tooltip_fmt").c_str(), mi + 1);
                ImGui::SetTooltip(tooltip);
            }
        }
    }
    
    // === ОБРАБОТКА КЛИКОВ ДЛЯ УСТАНОВКИ МЕТОК ===
    {
        // Проверяем что мышь над картой и не над UI элементами
        bool overUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
        if (ImGui::IsWindowHovered() && !overUI) {
            float baseImageSize = 2048.0f;
            float imgDisplaySize = baseImageSize * g_mapZoom;
            float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
            float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
            
            ImVec2 mousePos = ImGui::GetMousePos();
            
            // Проверяем что мышь над картинкой
            if (mousePos.x >= contentPos.x + imgX && mousePos.x <= contentPos.x + imgX + imgDisplaySize &&
                mousePos.y >= contentPos.y + imgY && mousePos.y <= contentPos.y + imgY + imgDisplaySize) {
                
                // Конвертируем экранные координаты в нормализованные (0-1)
                float normX = (mousePos.x - contentPos.x - imgX) / imgDisplaySize;
                float normY = (mousePos.y - contentPos.y - imgY) / imgDisplaySize;
                
                bool ctrlPressed = GetAsyncKeyState(VK_CONTROL) & 0x8000;
                
                // Ctrl+ЛКМ - добавляет метку (только если это клик, а не drag)
                if (ctrlPressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    // Проверяем что не было drag
                    if (!g_wasDragging) {
                        // Всегда добавляем метку без очистки предыдущих
                        MapMarker marker;
                        marker.x = normX;
                        marker.y = normY;
                        marker.r = 255;
                        marker.g = 200;
                        marker.b = 0;
                        
                        {
                            std::lock_guard<std::mutex> lock(g_mapMarkersMutex);
                            g_mapMarkers.push_back(marker);
                        }
                    }
                }
                
                // ЛКМ без Ctrl - выбор юнита (только если мышь в радиусе юнита)
                if (!ctrlPressed && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_wasDragging) {
                    // Радиус для клика (2% карты в нормализованных координатах)
                    float clickRadius = 0.002f;
                    
                    size_t foundIdx = SIZE_MAX;
                    
                    {
                        std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
                        for (size_t i = 0; i < g_mapObjects.size(); i++) {
                            const auto& obj = g_mapObjects[i];
                            if (obj.type == "airfield" || obj.isPlayer) continue;
                            
                            // Проверяем попадание в радиус юнита
                            float distX = obj.x - normX;
                            float distY = obj.y - normY;
                            float dist = sqrtf(distX * distX + distY * distY);
                            
                            if (dist <= clickRadius) {
                                foundIdx = i;
                                break; // Берем первый попавшийся в радиусе
                            }
                        }
                    }
                    
                    // Если мышь в радиусе юнита, обрабатываем клик
                    if (foundIdx != SIZE_MAX) {
                        bool isSelected = std::find(g_selectedUnits.begin(), g_selectedUnits.end(), foundIdx) != g_selectedUnits.end();
                        
                        // Всегда добавляем в выбранные (если еще не выбран)
                        if (!isSelected) {
                            g_selectedUnits.push_back(foundIdx);
                        }
                    }
                }
                
                // ПКМ - снять выделение с объекта под курсором (юнит или метка)
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    // Сначала проверяем метки
                    bool markerRemoved = false;
                    {
                        std::lock_guard<std::mutex> lock(g_mapMarkersMutex);
                        ImVec2 mousePos = ImGui::GetMousePos();
                        float baseImageSize = 2048.0f;
                        float imgDisplaySize = baseImageSize * g_mapZoom;
                        float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
                        float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
                        
                        for (int mi = (int)g_mapMarkers.size() - 1; mi >= 0; mi--) {
                            const auto& marker = g_mapMarkers[mi];
                            float markerScreenX = contentPos.x + imgX + marker.x * imgDisplaySize;
                            float markerScreenY = contentPos.y + imgY + marker.y * imgDisplaySize;
                            
                            float dx = mousePos.x - markerScreenX;
                            float dy = mousePos.y - markerScreenY;
                            float size = 8.0f;
                            bool markerHovered = (dx * dx + dy * dy < size * size * 2);
                            
                            if (markerHovered) {
                                g_mapMarkers.erase(g_mapMarkers.begin() + mi);
                                markerRemoved = true;
                                break;
                            }
                        }
                    }
                    
                    // Если метка не удалена, проверяем юниты
                    if (!markerRemoved) {
                        // Радиус для клика (2% карты в нормализованных координатах)
                        float clickRadius = 0.002f;
                        size_t foundIdx = SIZE_MAX;
                        
                        {
                            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
                            for (size_t i = 0; i < g_mapObjects.size(); i++) {
                                const auto& obj = g_mapObjects[i];
                                if (obj.type == "airfield" || obj.isPlayer) continue;
                                
                                // Проверяем попадание в радиус юнита
                                float distX = obj.x - normX;
                                float distY = obj.y - normY;
                                float dist = sqrtf(distX * distX + distY * distY);
                                
                                if (dist <= clickRadius) {
                                    foundIdx = i;
                                    break; // Берем первый попавшийся в радиусе
                                }
                            }
                        }
                        
                        // Если мышь в радиусе юнита, снимаем с него выделение
                        if (foundIdx != SIZE_MAX) {
                            auto it = std::find(g_selectedUnits.begin(), g_selectedUnits.end(), foundIdx);
                            if (it != g_selectedUnits.end()) {
                                g_selectedUnits.erase(it);
                                
                            }
                        }
                    }
                }
            }
        }
    }
    
    // === ОТРИСОВКА ЛИНИЙ И РАССТОЯНИЙ ===
    {
        float baseImageSize = 2048.0f;
        float imgDisplaySize = baseImageSize * g_mapZoom;
        float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
        float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Рисуем линии от игрока к выбранным юнитам
        {
            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
            std::lock_guard<std::mutex> lock2(g_mapInfoMutex);
            
            if (!g_selectedUnits.empty() && g_mapInfoData.valid) {
                // Находим игрока
                const MapObject* playerUnit = nullptr;
                for (const auto& obj : g_mapObjects) {
                    if (obj.isPlayer) {
                        playerUnit = &obj;
                        break;
                    }
                }
                
                if (playerUnit) {
                    float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                    float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                    
                    float playerScreenX = contentPos.x + imgX + playerUnit->x * imgDisplaySize;
                    float playerScreenY = contentPos.y + imgY + playerUnit->y * imgDisplaySize;
                    
                    float playerGameX = g_mapInfoData.mapMin[0] + playerUnit->x * mapSizeX;
                    float playerGameY = g_mapInfoData.mapMax[1] - playerUnit->y * mapSizeY;
                    
                    for (size_t selIdx : g_selectedUnits) {
                        if (selIdx >= g_mapObjects.size()) continue;
                        const auto& unit = g_mapObjects[selIdx];
                        
                        float unitScreenX = contentPos.x + imgX + unit.x * imgDisplaySize;
                        float unitScreenY = contentPos.y + imgY + unit.y * imgDisplaySize;
                        
                        // Рисуем линию (жёлтая для игрок-юнит)
                        drawList->AddLine(
                            ImVec2(playerScreenX, playerScreenY),
                            ImVec2(unitScreenX, unitScreenY),
                            IM_COL32(255, 255, 0, 180),
                            2.0f
                        );
                        
                        // Вычисляем дистанцию
                        float unitGameX = g_mapInfoData.mapMin[0] + unit.x * mapSizeX;
                        float unitGameY = g_mapInfoData.mapMax[1] - unit.y * mapSizeY;
                        
                        double distX = (double)unitGameX - (double)playerGameX;
                        double distY = (double)unitGameY - (double)playerGameY;
                        float distMeters = (float)sqrt(distX * distX + distY * distY);
                        float distKm = distMeters / 1000.0f;
                        
                        float midX = (playerScreenX + unitScreenX) * 0.5f;
                        float midY = (playerScreenY + unitScreenY) * 0.5f;
                        
                        char distText[32];
                        if (distKm >= 1.0f)
                            snprintf(distText, sizeof(distText), "%.3f km", distKm);
                        else
                            snprintf(distText, sizeof(distText), "%.0f m", distMeters);
                        
                        ImVec2 textSize = ImGui::CalcTextSize(distText);
                        drawList->AddRectFilled(
                            ImVec2(midX - textSize.x * 0.5f - 3, midY - textSize.y * 0.5f - 2),
                            ImVec2(midX + textSize.x * 0.5f + 3, midY + textSize.y * 0.5f + 2),
                            IM_COL32(0, 0, 0, 180),
                            3.0f
                        );
                        drawList->AddText(
                            ImVec2(midX - textSize.x * 0.5f, midY - textSize.y * 0.5f),
                            IM_COL32(255, 255, 0, 255),
                            distText
                        );
                    }
                }
            }
        }
        
        // Рисуем линии между выбранными юнитами
        {
            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
            std::lock_guard<std::mutex> lock2(g_mapInfoMutex);
            
            if (g_selectedUnits.size() >= 2 && g_mapInfoData.valid) {
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                
                for (size_t i = 0; i < g_selectedUnits.size(); i++) {
                    for (size_t j = i + 1; j < g_selectedUnits.size(); j++) {
                        size_t idx1 = g_selectedUnits[i];
                        size_t idx2 = g_selectedUnits[j];
                        
                        if (idx1 >= g_mapObjects.size() || idx2 >= g_mapObjects.size()) continue;
                        
                        const auto& unit1 = g_mapObjects[idx1];
                        const auto& unit2 = g_mapObjects[idx2];
                        
                        float unit1ScreenX = contentPos.x + imgX + unit1.x * imgDisplaySize;
                        float unit1ScreenY = contentPos.y + imgY + unit1.y * imgDisplaySize;
                        float unit2ScreenX = contentPos.x + imgX + unit2.x * imgDisplaySize;
                        float unit2ScreenY = contentPos.y + imgY + unit2.y * imgDisplaySize;
                        
                        // Рисуем линию (зелёная для юнит-юнит)
                        drawList->AddLine(
                            ImVec2(unit1ScreenX, unit1ScreenY),
                            ImVec2(unit2ScreenX, unit2ScreenY),
                            IM_COL32(100, 255, 100, 180),
                            2.0f
                        );
                        
                        // Вычисляем дистанцию
                        float unit1GameX = g_mapInfoData.mapMin[0] + unit1.x * mapSizeX;
                        float unit1GameY = g_mapInfoData.mapMax[1] - unit1.y * mapSizeY;
                        float unit2GameX = g_mapInfoData.mapMin[0] + unit2.x * mapSizeX;
                        float unit2GameY = g_mapInfoData.mapMax[1] - unit2.y * mapSizeY;
                        
                        double distX = (double)unit2GameX - (double)unit1GameX;
                        double distY = (double)unit2GameY - (double)unit1GameY;
                        float distMeters = (float)sqrt(distX * distX + distY * distY);
                        float distKm = distMeters / 1000.0f;
                        
                        float midX = (unit1ScreenX + unit2ScreenX) * 0.5f;
                        float midY = (unit1ScreenY + unit2ScreenY) * 0.5f;
                        
                        char distText[32];
                        if (distKm >= 1.0f)
                            snprintf(distText, sizeof(distText), "%.3f km", distKm);
                        else
                            snprintf(distText, sizeof(distText), "%.0f m", distMeters);
                        
                        ImVec2 textSize = ImGui::CalcTextSize(distText);
                        drawList->AddRectFilled(
                            ImVec2(midX - textSize.x * 0.5f - 3, midY - textSize.y * 0.5f - 2),
                            ImVec2(midX + textSize.x * 0.5f + 3, midY + textSize.y * 0.5f + 2),
                            IM_COL32(0, 0, 0, 180),
                            3.0f
                        );
                        drawList->AddText(
                            ImVec2(midX - textSize.x * 0.5f, midY - textSize.y * 0.5f),
                            IM_COL32(100, 255, 100, 255),
                            distText
                        );
                    }
                }
            }
        }
        
        // Рисуем линии от игрока к меткам
        {
            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
            std::lock_guard<std::mutex> lock2(g_mapMarkersMutex);
            std::lock_guard<std::mutex> lock3(g_mapInfoMutex);
            
            if (!g_mapMarkers.empty() && g_mapInfoData.valid) {
                // Находим игрока
                const MapObject* playerUnit = nullptr;
                for (const auto& obj : g_mapObjects) {
                    if (obj.isPlayer) {
                        playerUnit = &obj;
                        break;
                    }
                }
                
                if (playerUnit) {
                    float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                    float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                    
                    float playerScreenX = contentPos.x + imgX + playerUnit->x * imgDisplaySize;
                    float playerScreenY = contentPos.y + imgY + playerUnit->y * imgDisplaySize;
                    
                    float playerGameX = g_mapInfoData.mapMin[0] + playerUnit->x * mapSizeX;
                    float playerGameY = g_mapInfoData.mapMax[1] - playerUnit->y * mapSizeY;
                    
                    for (const auto& marker : g_mapMarkers) {
                        float markerScreenX = contentPos.x + imgX + marker.x * imgDisplaySize;
                        float markerScreenY = contentPos.y + imgY + marker.y * imgDisplaySize;
                        
                        // Рисуем линию (оранжевая для меток)
                        drawList->AddLine(
                            ImVec2(playerScreenX, playerScreenY),
                            ImVec2(markerScreenX, markerScreenY),
                            IM_COL32(255, 150, 0, 180),
                            2.0f
                        );
                        
                        // Вычисляем дистанцию
                        float markerGameX = g_mapInfoData.mapMin[0] + marker.x * mapSizeX;
                        float markerGameY = g_mapInfoData.mapMax[1] - marker.y * mapSizeY;
                        
                        double distX = (double)markerGameX - (double)playerGameX;
                        double distY = (double)markerGameY - (double)playerGameY;
                        float distMeters = (float)sqrt(distX * distX + distY * distY);
                        float distKm = distMeters / 1000.0f;
                        
                        float midX = (playerScreenX + markerScreenX) * 0.5f;
                        float midY = (playerScreenY + markerScreenY) * 0.5f;
                        
                        char distText[32];
                        if (distKm >= 1.0f)
                            snprintf(distText, sizeof(distText), "%.3f km", distKm);
                        else
                            snprintf(distText, sizeof(distText), "%.0f m", distMeters);
                        
                        ImVec2 textSize = ImGui::CalcTextSize(distText);
                        drawList->AddRectFilled(
                            ImVec2(midX - textSize.x * 0.5f - 3, midY - textSize.y * 0.5f - 2),
                            ImVec2(midX + textSize.x * 0.5f + 3, midY + textSize.y * 0.5f + 2),
                            IM_COL32(0, 0, 0, 180),
                            3.0f
                        );
                        drawList->AddText(
                            ImVec2(midX - textSize.x * 0.5f, midY - textSize.y * 0.5f),
                            IM_COL32(255, 150, 0, 255),
                            distText
                        );
                    }
                }
            }
            
            // Рисуем линии между выбранными юнитами и метками
            if (!g_selectedUnits.empty() && !g_mapMarkers.empty() && g_mapInfoData.valid) {
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                
                for (size_t selIdx : g_selectedUnits) {
                    if (selIdx >= g_mapObjects.size()) continue;
                    const auto& unit = g_mapObjects[selIdx];
                    
                    float unitScreenX = contentPos.x + imgX + unit.x * imgDisplaySize;
                    float unitScreenY = contentPos.y + imgY + unit.y * imgDisplaySize;
                    
                    float unitGameX = g_mapInfoData.mapMin[0] + unit.x * mapSizeX;
                    float unitGameY = g_mapInfoData.mapMax[1] - unit.y * mapSizeY;
                    
                    for (const auto& marker : g_mapMarkers) {
                        float markerScreenX = contentPos.x + imgX + marker.x * imgDisplaySize;
                        float markerScreenY = contentPos.y + imgY + marker.y * imgDisplaySize;
                        
                        // Рисуем линию (голубая для юнит-метка)
                        drawList->AddLine(
                            ImVec2(unitScreenX, unitScreenY),
                            ImVec2(markerScreenX, markerScreenY),
                            IM_COL32(0, 200, 255, 180),
                            2.0f
                        );
                        
                        // Вычисляем дистанцию
                        float markerGameX = g_mapInfoData.mapMin[0] + marker.x * mapSizeX;
                        float markerGameY = g_mapInfoData.mapMax[1] - marker.y * mapSizeY;
                        
                        double distX = (double)markerGameX - (double)unitGameX;
                        double distY = (double)markerGameY - (double)unitGameY;
                        float distMeters = (float)sqrt(distX * distX + distY * distY);
                        float distKm = distMeters / 1000.0f;
                        
                        float midX = (unitScreenX + markerScreenX) * 0.5f;
                        float midY = (unitScreenY + markerScreenY) * 0.5f;
                        
                        char distText[32];
                        if (distKm >= 1.0f)
                            snprintf(distText, sizeof(distText), "%.3f km", distKm);
                        else
                            snprintf(distText, sizeof(distText), "%.0f m", distMeters);
                        
                        ImVec2 textSize = ImGui::CalcTextSize(distText);
                        drawList->AddRectFilled(
                            ImVec2(midX - textSize.x * 0.5f - 3, midY - textSize.y * 0.5f - 2),
                            ImVec2(midX + textSize.x * 0.5f + 3, midY + textSize.y * 0.5f + 2),
                            IM_COL32(0, 0, 0, 180),
                            3.0f
                        );
                        drawList->AddText(
                            ImVec2(midX - textSize.x * 0.5f, midY - textSize.y * 0.5f),
                            IM_COL32(0, 200, 255, 255),
                            distText
                        );
                    }
                }
            }
            
            // Рисуем линии между метками (если их несколько)
            if (g_mapMarkers.size() >= 2 && g_mapInfoData.valid) {
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                
                for (size_t i = 0; i < g_mapMarkers.size(); i++) {
                    for (size_t j = i + 1; j < g_mapMarkers.size(); j++) {
                        const auto& marker1 = g_mapMarkers[i];
                        const auto& marker2 = g_mapMarkers[j];
                        
                        float marker1ScreenX = contentPos.x + imgX + marker1.x * imgDisplaySize;
                        float marker1ScreenY = contentPos.y + imgY + marker1.y * imgDisplaySize;
                        float marker2ScreenX = contentPos.x + imgX + marker2.x * imgDisplaySize;
                        float marker2ScreenY = contentPos.y + imgY + marker2.y * imgDisplaySize;
                        
                        // Рисуем линию (жёлтая для метка-метка)
                        drawList->AddLine(
                            ImVec2(marker1ScreenX, marker1ScreenY),
                            ImVec2(marker2ScreenX, marker2ScreenY),
                            IM_COL32(255, 200, 0, 150),
                            1.5f
                        );
                        
                        // Вычисляем дистанцию
                        float marker1GameX = g_mapInfoData.mapMin[0] + marker1.x * mapSizeX;
                        float marker1GameY = g_mapInfoData.mapMax[1] - marker1.y * mapSizeY;
                        float marker2GameX = g_mapInfoData.mapMin[0] + marker2.x * mapSizeX;
                        float marker2GameY = g_mapInfoData.mapMax[1] - marker2.y * mapSizeY;
                        
                        double distX = (double)marker2GameX - (double)marker1GameX;
                        double distY = (double)marker2GameY - (double)marker1GameY;
                        float distMeters = (float)sqrt(distX * distX + distY * distY);
                        float distKm = distMeters / 1000.0f;
                        
                        float midX = (marker1ScreenX + marker2ScreenX) * 0.5f;
                        float midY = (marker1ScreenY + marker2ScreenY) * 0.5f;
                        
                        char distText[32];
                        if (distKm >= 1.0f)
                            snprintf(distText, sizeof(distText), "%.3f km", distKm);
                        else
                            snprintf(distText, sizeof(distText), "%.0f m", distMeters);
                        
                        ImVec2 textSize = ImGui::CalcTextSize(distText);
                        drawList->AddRectFilled(
                            ImVec2(midX - textSize.x * 0.5f - 3, midY - textSize.y * 0.5f - 2),
                            ImVec2(midX + textSize.x * 0.5f + 3, midY + textSize.y * 0.5f + 2),
                            IM_COL32(0, 0, 0, 180),
                            3.0f
                        );
                        drawList->AddText(
                            ImVec2(midX - textSize.x * 0.5f, midY - textSize.y * 0.5f),
                            IM_COL32(255, 200, 0, 255),
                            distText
                        );
                    }
                }
            }
        }
    }
    
    // === ОТРИСОВКА ДЕБАГ ИНФОРМАЦИИ ===
    if (g_debugMode) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 contentScreenPos = ImGui::GetWindowPos();
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        
        float baseImageSize = 2048.0f;
        float imgDisplaySize = baseImageSize * g_mapZoom;
        float imgX = contentSize.x * 0.5f + g_mapOffsetX - imgDisplaySize * 0.5f;
        float imgY = contentSize.y * 0.5f + g_mapOffsetY - imgDisplaySize * 0.5f;
        
        // Вычисляем позицию курсора относительно карты
        float mouseRelX = mousePos.x - contentScreenPos.x - imgX;
        float mouseRelY = mousePos.y - contentScreenPos.y - imgY;
        
        // Нормализованные координаты (0-1)
        float normalizedX = mouseRelX / imgDisplaySize;
        float normalizedY = mouseRelY / imgDisplaySize;
        
        // Игровые координаты (из map_info)
        float gameX = 0.0f, gameY = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_mapInfoMutex);
            if (g_mapInfoData.valid) {
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                gameX = g_mapInfoData.mapMin[0] + normalizedX * mapSizeX;
                gameY = g_mapInfoData.mapMax[1] - normalizedY * mapSizeY; // Y инвертирован
            }
        }
        
        // Ищем юнит под курсором
        const MapObject* hoveredUnit = nullptr;
        float minDistance = 20.0f; // Минимальное расстояние в пикселях
        {
            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
            for (const auto& obj : g_mapObjects) {
                if (obj.type == "airfield") continue; // Пропускаем аэродромы
                
                // Вычисляем позицию юнита на экране
                float objScreenX = contentScreenPos.x + imgX + obj.x * imgDisplaySize;
                float objScreenY = contentScreenPos.y + imgY + obj.y * imgDisplaySize;
                
                // Расстояние от курсора до юнита
                float dx = mousePos.x - objScreenX;
                float dy = mousePos.y - objScreenY;
                float distance = sqrtf(dx * dx + dy * dy);
                
                if (distance < minDistance) {
                    minDistance = distance;
                    hoveredUnit = &obj;
                }
            }
        }
        
        // Находим игрока для вычисления расстояния
        const MapObject* playerUnit = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mapObjectsMutex);
            for (const auto& obj : g_mapObjects) {
                if (obj.isPlayer) {
                    playerUnit = &obj;
                    break;
                }
            }
        }
        
        // Функция для вычисления координат сетки (буква + цифра)
        auto getGridCoordinates = [](float gameX, float gameY) -> std::string {
            std::lock_guard<std::mutex> lock(g_mapInfoMutex);
            if (!g_mapInfoData.valid) return "-";
            
            char gridStr[32];
            
            if (g_mapInfoData.hudType == 0) {
                // hud_type = 0 (авиа): сетка от левого верхнего угла
                // Вычисляем позицию относительно mapMin
                float offsetX = gameX - g_mapInfoData.mapMin[0];
                float offsetY = g_mapInfoData.mapMax[1] - gameY; // Y инвертирован
                
                // Вычисляем номер ячейки
                int cellX = (int)std::floor(offsetX / g_mapInfoData.gridSteps[0]);
                int cellY = (int)std::floor(offsetY / g_mapInfoData.gridSteps[1]);
                
                // X начинается с 1, Y с 0 (A, B, C...)
                int letterIndex = cellY;
                if (letterIndex < 0) letterIndex = 0;
                if (letterIndex >= 26) {
                    // Поддержка двойных букв (AA, AB...)
                    int firstLetter = (letterIndex / 26) - 1;
                    int secondLetter = letterIndex % 26;
                    if (firstLetter >= 0 && firstLetter < 26 && secondLetter >= 0 && secondLetter < 26) {
                        snprintf(gridStr, sizeof(gridStr), "%c%c%d", 'A' + firstLetter, 'A' + secondLetter, cellX + 1);
                    } else {
                        snprintf(gridStr, sizeof(gridStr), "A%d", cellX + 1);
                    }
                } else {
                    snprintf(gridStr, sizeof(gridStr), "%c%d", 'A' + letterIndex, cellX + 1);
                }
            } else {
                // hud_type = 1 (танки): сетка от grid_zero
                // Вычисляем смещение от grid_zero
                float offsetX = gameX - g_mapInfoData.gridZero[0];
                float offsetY = gameY - g_mapInfoData.gridZero[1];
                
                // Вычисляем номер ячейки
                int cellX = (int)std::floor(offsetX / g_mapInfoData.gridSteps[0]);
                int cellY = (int)std::floor(offsetY / g_mapInfoData.gridSteps[1]);
                
                // Вычисляем индекс первой ячейки (как в отрисовке сетки)
                float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                float origPixelsPerUnitX = 2048.0f / mapSizeX;
                float origPixelsPerUnitY = 2048.0f / mapSizeY;
                float origGridStepX = g_mapInfoData.gridSteps[0] * origPixelsPerUnitX;
                float origGridStepY = g_mapInfoData.gridSteps[1] * origPixelsPerUnitY;
                float origGridZeroX = (g_mapInfoData.gridZero[0] - g_mapInfoData.mapMin[0]) * origPixelsPerUnitX;
                float origGridZeroY = (g_mapInfoData.mapMax[1] - g_mapInfoData.gridZero[1]) * origPixelsPerUnitY;
                
                float cellsFromLeftToZero = origGridZeroX / origGridStepX;
                float cellsFromTopToZero = origGridZeroY / origGridStepY;
                
                int indexAtLeftEdge = 1 - (int)std::floor(cellsFromLeftToZero);
                int indexAtTopEdge = -(int)std::floor(cellsFromTopToZero);
                
                // Корректируем индексы
                int finalX = indexAtLeftEdge + cellX;
                int finalY = indexAtTopEdge + cellY;
                
                if (finalY < 0) finalY = 0;
                if (finalY >= 26) {
                    int firstLetter = (finalY / 26) - 1;
                    int secondLetter = finalY % 26;
                    if (firstLetter >= 0 && firstLetter < 26 && secondLetter >= 0 && secondLetter < 26) {
                        snprintf(gridStr, sizeof(gridStr), "%c%c%d", 'A' + firstLetter, 'A' + secondLetter, finalX);
                    } else {
                        snprintf(gridStr, sizeof(gridStr), "A%d", finalX);
                    }
                } else {
                    snprintf(gridStr, sizeof(gridStr), "%c%d", 'A' + finalY, finalX);
                }
            }
            
            return std::string(gridStr);
        };
        
        // Отрисовка дебаг информации
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Текст дебаг информации (старый формат)
        std::vector<std::string> debugLines;
        std::string gridCoords = getGridCoordinates(gameX, gameY);
        
        if (hoveredUnit) {
            // Вычисляем игровые координаты юнита
            float unitGameX = 0.0f, unitGameY = 0.0f;
            {
                std::lock_guard<std::mutex> lock(g_mapInfoMutex);
                if (g_mapInfoData.valid) {
                    float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                    float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                    unitGameX = g_mapInfoData.mapMin[0] + hoveredUnit->x * mapSizeX;
                    unitGameY = g_mapInfoData.mapMax[1] - hoveredUnit->y * mapSizeY;
                }
            }
            
            std::string unitGridCoords = getGridCoordinates(unitGameX, unitGameY);
            
            // Информация о юните (старый формат)
            char line1[256], line2[256], line3[256], line4[256];
            snprintf(line1, sizeof(line1), TR().Get("unit_label_fmt").c_str(), hoveredUnit->icon.c_str());
            debugLines.push_back(line1);
            
            snprintf(line2, sizeof(line2), TR().Get("type_label_fmt").c_str(), hoveredUnit->type.c_str());
            debugLines.push_back(line2);
            
            snprintf(line3, sizeof(line3), TR().Get("grid_label_fmt").c_str(), unitGridCoords.c_str());
            debugLines.push_back(line3);
            
            snprintf(line4, sizeof(line4), TR().Get("position_fmt").c_str(), unitGameX, unitGameY);
            debugLines.push_back(line4);
            
            // Вычисляем расстояние до игрока
            if (playerUnit) {
                float playerGameX = 0.0f, playerGameY = 0.0f;
                {
                    std::lock_guard<std::mutex> lock(g_mapInfoMutex);
                    if (g_mapInfoData.valid) {
                        float mapSizeX = g_mapInfoData.mapMax[0] - g_mapInfoData.mapMin[0];
                        float mapSizeY = g_mapInfoData.mapMax[1] - g_mapInfoData.mapMin[1];
                        playerGameX = g_mapInfoData.mapMin[0] + playerUnit->x * mapSizeX;
                        playerGameY = g_mapInfoData.mapMax[1] - playerUnit->y * mapSizeY;
                    }
                }
                
                float dx = unitGameX - playerGameX;
                float dy = unitGameY - playerGameY;
                float distance = sqrtf(dx * dx + dy * dy);
                
                char line5[256];
                snprintf(line5, sizeof(line5), TR().Get("distance_to_player_fmt").c_str(), distance);
                debugLines.push_back(line5);
            }
        } else {
            // Информация о позиции курсора (старый формат)
            char line1[256], line2[256], line3[256];
            snprintf(line1, sizeof(line1), TR().Get("cursor_grid_fmt").c_str(), gridCoords.c_str());
            debugLines.push_back(line1);
            
            snprintf(line2, sizeof(line2), TR().Get("cursor_game_coord_fmt").c_str(), gameX, gameY);
            debugLines.push_back(line2);
            
            snprintf(line3, sizeof(line3), TR().Get("cursor_pixel_coord_fmt").c_str(), mouseRelX, mouseRelY);
            debugLines.push_back(line3);
        }
        
        // Вычисляем размер панели на основе текста
        float maxLineWidth = 0.0f;
        float lineHeight = ImGui::GetTextLineHeight();
        for (const auto& line : debugLines) {
            ImVec2 textSize = ImGui::CalcTextSize(line.c_str());
            if (textSize.x > maxLineWidth) {
                maxLineWidth = textSize.x;
            }
        }
        
        const float padding = 10.0f;
        float debugTextWidth = maxLineWidth + padding * 2.0f;
        float debugTextHeight = debugLines.size() * lineHeight + padding * 2.0f;
        
        // Позиционирование с учетом границ окна
        float debugTextX = mousePos.x + 15.0f;
        float debugTextY = mousePos.y + 15.0f;
        
        // Проверяем, не выходит ли панель за правую границу
        if (debugTextX + debugTextWidth > contentScreenPos.x + contentSize.x) {
            debugTextX = mousePos.x - debugTextWidth - 15.0f; // Слева от курсора
        }
        
        // Проверяем, не выходит ли панель за нижнюю границу
        if (debugTextY + debugTextHeight > contentScreenPos.y + contentSize.y) {
            debugTextY = mousePos.y - debugTextHeight - 15.0f; // Над курсором
        }
        
        // Проверяем, не выходит ли панель за левую границу
        if (debugTextX < contentScreenPos.x) {
            debugTextX = contentScreenPos.x + 5.0f;
        }
        
        // Проверяем, не выходит ли панель за верхнюю границу
        if (debugTextY < contentScreenPos.y) {
            debugTextY = contentScreenPos.y + 5.0f;
        }
        
        // Рисуем фон
        drawList->AddRectFilled(
            ImVec2(debugTextX - padding, debugTextY - padding),
            ImVec2(debugTextX + debugTextWidth - padding, debugTextY + debugTextHeight - padding),
            IM_COL32(0, 0, 0, 200),
            3.0f
        );
        drawList->AddRect(
            ImVec2(debugTextX - padding, debugTextY - padding),
            ImVec2(debugTextX + debugTextWidth - padding, debugTextY + debugTextHeight - padding),
            IM_COL32(255, 255, 255, 255),
            3.0f,
            0,
            2.0f
        );
        
        // Отрисовываем строки текста
        float lineY = debugTextY;
        for (const auto& line : debugLines) {
            drawList->AddText(ImVec2(debugTextX, lineY), IM_COL32(255, 255, 255, 255), line.c_str());
            lineY += lineHeight;
        }
        
        // Квадрат под курсором
        float squareSize = 10.0f;
        drawList->AddRect(
            ImVec2(mousePos.x - squareSize, mousePos.y - squareSize),
            ImVec2(mousePos.x + squareSize, mousePos.y + squareSize),
            IM_COL32(255, 255, 0, 255),
            0.0f,
            0,
            2.0f
        );
    }
    
    // === CONTENT 1 (левая боковая панель) ===
    ImGui::SetCursorPos(ImVec2(padding, padding));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowWidth * 0.005f); // Пропорциональные закругления
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding)); // Пропорциональные отступы
    
    ImGui::BeginChild("##Content1", ImVec2(sidebarWidth, contentHeight - padding * 2), true);
    
    // Определяем тип техники по indicators
    bool isTank = false;
    std::string vehicleName = "";
    {
        std::lock_guard<std::mutex> lock(g_indicatorsMutex);
        if (g_indicatorsData.valid && !g_indicatorsData.type.empty()) {
            // Если тип начинается с "tankModels/" - это танк
            if (g_indicatorsData.type.find("tankModels/") == 0) {
                isTank = true;
                // Извлекаем название танка без префикса "tankModels/"
                vehicleName = g_indicatorsData.type.substr(11); // 11 = длина "tankModels/"
            } else {
                vehicleName = g_indicatorsData.type;
            }
        }
        
        if (!vehicleName.empty()) {
            if (isTank) {
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), TR().Get("tank_data_fmt").c_str(), vehicleName.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), TR().Get("aircraft_data_fmt").c_str(), vehicleName.c_str());
            }
        } else {
            if (isTank) {
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), TR().Get("tank_data").c_str());
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), TR().Get("aircraft_data").c_str());
            }
        }
    }
    ImGui::Separator();
    ImGui::Spacing();
    
    // Функция для парсинга float из JSON
    auto parseJsonFloat = [](const std::string& str, const std::string& key) -> float {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                return static_cast<float>(std::atof(str.substr(pos).c_str()));
            }
        }
        return 0.0f;
    };
    
    // Функция для парсинга int из JSON
    auto parseJsonInt = [](const std::string& str, const std::string& key) -> int {
        size_t pos = str.find("\"" + key + "\"");
        if (pos != std::string::npos) {
            pos = str.find(":", pos);
            if (pos != std::string::npos) {
                pos++;
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
                    pos++;
                return std::atoi(str.substr(pos).c_str());
            }
        }
        return 0;
    };
    
    // Отображаем данные Indicators
    {
        std::lock_guard<std::mutex> lock(g_indicatorsMutex);
        if (g_indicatorsData.valid) {
            bool isTankType = false;
            std::string vehicleType = g_indicatorsData.type;
            if (!vehicleType.empty() && vehicleType.find("tankModels/") == 0) {
                isTankType = true;
            }
            
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), TR().Get("indicators_header").c_str());
            
            // Общие параметры для всех типов техники
            if (g_indicatorsData.speed > 0.0f) {
                ImGui::Text(TR().Get("speed_data_fmt").c_str(), g_indicatorsData.speed);
            }
            
            if (g_indicatorsData.fuel > 0.0f) {
                ImGui::Text(TR().Get("fuel_data_fmt").c_str(), g_indicatorsData.fuel);
            }
            
            if (g_indicatorsData.throttle > 0.0f) {
                ImGui::Text(TR().Get("throttle_data_fmt").c_str(), g_indicatorsData.throttle * 100.0f);
            }
            
            // Параметры только для самолетов
            if (!isTankType) {
                if (g_indicatorsData.altitude_hour > 0.0f) {
                    ImGui::Text(TR().Get("altitude_fmt_m").c_str(), g_indicatorsData.altitude_hour);
                }
                
                if (g_indicatorsData.compass >= 0.0f) {
                    ImGui::Text(TR().Get("compass_data_fmt").c_str(), g_indicatorsData.compass);
                }
                
                if (g_indicatorsData.mach > 0.0f) {
                    ImGui::Text(TR().Get("mach_data_fmt").c_str(), g_indicatorsData.mach);
                }
                
                if (g_indicatorsData.g_meter != 0.0f) {
                    ImGui::Text(TR().Get("g_meter_data_fmt").c_str(), g_indicatorsData.g_meter);
                }
                
                // Шасси: 0 = выпущено, 50 = в процессе, 100 = убрано
                float gearsPercent = g_indicatorsData.gears * 100.0f;
                if (gearsPercent > 0.01f) {
                    std::string gearsStatus;

                    if (gearsPercent < 10.0f) {
                        gearsStatus = TR().Get("gear_released_state");
                    }
                    else if (gearsPercent > 90.0f) {
                        gearsStatus = TR().Get("gear_retracted_state");
                    }
                    else {
                        gearsStatus = TR().Get("gear_in_progress_state");
                    }

                    ImGui::Text(
                        TR().Get("gears_status_fmt").c_str(),
                        gearsPercent,
                        gearsStatus.c_str()
                    );
                }
                
                if (g_indicatorsData.flaps > 0.01f) {
                    ImGui::Text(TR().Get("flaps_fmt").c_str(), g_indicatorsData.flaps * 100.0f);
                }
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), TR().Get("indicators_no_data").c_str());
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Отображаем данные State
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_stateData.valid) {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), TR().Get("state_header").c_str());
            ImGui::Text(TR().Get("altitude_fmt").c_str(), g_stateData.altitude);
            ImGui::Text(TR().Get("tas_fmt").c_str(), g_stateData.tas);
            ImGui::Text(TR().Get("ias_fmt").c_str(), g_stateData.ias);
            ImGui::Text(TR().Get("mach_fmt").c_str(), g_stateData.mach);
            ImGui::Text(TR().Get("aoa_fmt").c_str(), g_stateData.aoa);
            ImGui::Text(TR().Get("vy_fmt").c_str(), g_stateData.vy);
            ImGui::Text(TR().Get("fuel_fmt").c_str(), g_stateData.fuel, g_stateData.fuel0);
            float fuelPercent = g_stateData.fuel0 > 0 ? (g_stateData.fuel * 100.0f / g_stateData.fuel0) : 0.0f;
            ImGui::Text(TR().Get("fuel_percent_fmt").c_str(), fuelPercent);
            ImGui::Text(TR().Get("throttle1_fmt").c_str(), g_stateData.throttle1);
            ImGui::Text(TR().Get("rpm1_fmt").c_str(), g_stateData.rpm1);
            ImGui::Text(TR().Get("power1_fmt").c_str(), g_stateData.power1);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), TR().Get("state_no_data").c_str());
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Отображаем данные Mission
    {
        std::lock_guard<std::mutex> lock(g_missionMutex);
        if (g_missionData.valid) {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), TR().Get("mission_header").c_str());
            
            // Статус миссии
            std::string statusText = TR().Get("status_label");
            if (g_missionData.status == "running") {
                statusText += TR().Get("status_running");
            } else if (g_missionData.status == "fail") {
                statusText += TR().Get("status_fail");
            } else {
                statusText += g_missionData.status;
            }
            ImGui::Text("%s", statusText.c_str());
            
            ImGui::Spacing();
            
            // Цели миссии
            if (!g_missionData.objectives.empty()) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), TR().Get("objectives_label").c_str());
                for (const auto& obj : g_missionData.objectives) {
                    if (obj.primary) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), TR().Get("objective_primary").c_str());
                    } else {
                        ImGui::Text(TR().Get("objective_secondary").c_str());
                    }
                    
                    // Статус цели
                    std::string statusStr = TR().Get("objective_status_label");
                    if (obj.status == "in_progress") {
                        statusStr += TR().Get("status_in_progress");
                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", statusStr.c_str());
                    } else if (obj.status == "completed") {
                        statusStr += TR().Get("status_completed");
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", statusStr.c_str());
                    } else if (obj.status == "failed") {
                        statusStr += TR().Get("status_failed");
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", statusStr.c_str());
                    } else {
                        statusStr += obj.status;
                        ImGui::Text("%s", statusStr.c_str());
                    }
                    
                    // Текст цели
                    if (!obj.text.empty()) {
                        ImGui::Text(TR().Get("objective_text_fmt").c_str(), obj.text.c_str());
                    }
                    ImGui::Spacing();
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), TR().Get("no_objectives").c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), TR().Get("mission_no_data").c_str());
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Кнопка для скрытия/показа чата (Content 2)
    std::string chatButtonText = g_content2Visible ? TR().Get("hide_chat") : TR().Get("show_chat");
    if (ImGui::Button(chatButtonText.c_str(), ImVec2(-1, 0))) {
        g_content2Visible = !g_content2Visible;
        // Сохраняем настройки при изменении
        SaveSettings();
    }
    
    ImGui::Spacing();
    
    // Кнопка для переключения дебаг режима
    std::string debugButtonText = g_debugMode ? TR().Get("debug_on") : TR().Get("debug_off");
    if (ImGui::Button(debugButtonText.c_str(), ImVec2(-1, 0))) {
        g_debugMode = !g_debugMode;
    }
    
    ImGui::Spacing();
    
    // Кнопка для переключения режима слежения
    if (g_followMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 0.9f));
    }

    std::string followButtonText = g_followMode ? TR().Get("follow_on") : TR().Get("follow_off");
    if (ImGui::Button(followButtonText.c_str(), ImVec2(-1, 0))) {
        g_followMode = !g_followMode;
        if (g_followMode) {
            // Сбрасываем корректировку зума при включении
            g_followZoomAdjust = 1.0f;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR().Get("follow_tooltip").c_str());
    }
    if (g_followMode) {
        ImGui::PopStyleColor();
    }
    
    ImGui::Spacing();
    
    // Кнопка для очистки всех выделений и меток
    if (ImGui::Button(TR().Get("clear_tooltip").c_str(), ImVec2(-1, 0))) {
        g_selectedUnits.clear();
        {
            std::lock_guard<std::mutex> lock(g_mapMarkersMutex);
            g_mapMarkers.clear();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR().Get("follow_tooltip").c_str());
    }
    
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    
    // === CONTENT 2 (правый нижний угол) ===
    // Отображаем Content 2 только если он видим
    if (g_content2Visible) {
        // Фиксированный размер для Content 2 в правом нижнем углу (уменьшен в 2 раза)
        const float content2FixedWidth = (windowWidth * 0.45f * 1.75f) / 2.0f; // Уменьшено в 2 раза
        const float content2FixedHeight = (windowHeight * 0.4f / 1.5f) / 2.0f; // Уменьшено в 2 раза
        
        // Позиция в правом нижнем углу
        float content2X = contentWidth - content2FixedWidth - padding;
        float content2Y = contentHeight - content2FixedHeight - padding;
        
        // Content 2 в правом нижнем углу
        ImGui::SetCursorPos(ImVec2(content2X, content2Y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowWidth * 0.005f); // Пропорциональные закругления
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding * 0.5f, padding * 0.3f)); // Уменьшенные отступы (низкопрофильный)
        
        ImGui::BeginChild("##Content2", ImVec2(content2FixedWidth, content2FixedHeight), true);
        
        // Табы для чата (низкопрофильные)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding * 0.3f, padding * 0.15f)); // Уменьшенные отступы по Y
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.15f, 0.15f, 0.18f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.20f, 0.20f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.20f, 0.20f, 0.24f, 1.0f));
        
        if (ImGui::BeginTabBar("##Content2Tabs", ImGuiTabBarFlags_None)) {
            // Таб "Чат"
            if (ImGui::BeginTabItem(TR().Get("chat_tab").c_str())) {
                ImGui::BeginChild("##ChatList", ImVec2(0, -1), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                
                std::lock_guard<std::mutex> lock(g_chatMutex);
                
                // Цикл парсинга сообщений (без вывода)
                if (!g_chatMessages.empty()) {
                    // Проходим по всем сообщениям для обработки/парсинга
                    for (auto it = g_chatMessages.rbegin(); it != g_chatMessages.rend(); ++it) {
                        const auto& msg = *it;
                        

                        int id = msg.id;
                        std::string sender = msg.sender;
                        std::string message = msg.msg;
                        bool enemy = msg.enemy;
                        std::string mode = msg.mode;

                        // Форматирование строки через sprintf
                        char formattedString[512];
                        snprintf(formattedString, sizeof(formattedString), 
                            "#7a7d81[%s]  %s[%s] #e9edf5 %s",

                            mode.c_str(),
                            enemy ? "#602c30" : "#354e98",
                            sender.c_str(),
                            message.c_str()
                        );
                        std::string resultString = formattedString;
                        
                        // Отображаем текст с поддержкой цветовых кодов #RRGGBB или #AARRGGBB
                        // Пример: "Обычный текст #FF0000красный текст #00FF00зеленый текст"
                        // Можно добавить цвета прямо в строку: "#FF0000Красный текст #00FF00Зеленый текст"
                        RenderColoredText(resultString);

                        ImGui::Separator();
                    }
                }
                
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            
            // Таб "Сражения"
            if (ImGui::BeginTabItem(TR().Get("events_tab").c_str())) {
                ImGui::BeginChild("##EventList", ImVec2(0, -1), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                
                std::lock_guard<std::mutex> lock(g_eventMutex);
                if (g_eventMessages.empty()) {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), TR().Get("no_events").c_str());
                } else {
                    // Отображаем события в обратном порядке (новые сверху)
                    for (auto it = g_eventMessages.rbegin(); it != g_eventMessages.rend(); ++it) {
                        const auto& msg = *it;
                        
                        // Цвет текста в зависимости от типа
                        ImVec4 textColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                        if (msg.enemy) {
                            textColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Красный для врагов
                        } else if (msg.msg.find("destroyed") != std::string::npos || 
                                   msg.msg.find("уничтожил") != std::string::npos ||
                                   msg.msg.find("set afire") != std::string::npos) {
                            textColor = ImVec4(0.6f, 0.9f, 0.6f, 1.0f); // Зеленый для боевых событий
                        }
                        
                        // Отображаем сообщение с поддержкой цветовых тегов <color=#RRGGBBAA>текст</color>
                        // Если в сообщении нет цветовых тегов, используем цвет по умолчанию
                        if (msg.msg.find("<color=") == std::string::npos) {
                            ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                            RenderColoredText(msg.msg);
                            ImGui::PopStyleColor();
                        } else {
                            RenderColoredText(msg.msg);
                        }
                        
                        ImGui::Separator();
                    }
                }
                
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
        
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    } // Конец if (content2Visible)
    
    // Центр остается свободным для основного контента
    
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// Очистка UI
void ShutdownUI()
{
    // Сохраняем настройки при закрытии
    SaveSettings();
    
    // Освобождаем текстуру подложки
    if (g_backgroundTexture) {
        g_backgroundTexture->Release();
        g_backgroundTexture = nullptr;
    }
}

// Сохранение настроек в файл
void SaveSettings()
{
    if (!g_hWnd)
        return;
    
    // Получаем путь к файлу настроек (в папке с exe)
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string configPath = exePath;
    size_t lastSlash = configPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        configPath = configPath.substr(0, lastSlash + 1) + "config.ini";
    } else {
        configPath = "config.ini";
    }
    
    // Получаем позицию и размер окна
    RECT windowRect;
    GetWindowRect(g_hWnd, &windowRect);
    int windowX = windowRect.left;
    int windowY = windowRect.top;
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;
    
    // Определяем монитор, на котором находится окно
    HMONITOR hMonitor = MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    int monitorIndex = 0;
    int monitorCenterX = 0, monitorCenterY = 0; // Координаты центра монитора для надежной идентификации
    
    // Находим индекс монитора и сохраняем его координаты
    int monitorCount = GetSystemMetrics(SM_CMONITORS);
    HMONITOR monitors[16] = { nullptr }; // Максимум 16 мониторов
    int actualCount = 0;
    
    if (monitorCount <= 16) {
        struct MonitorEnumData {
            HMONITOR monitors[16];
            int count = 0;
        } enumData;
        
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            MonitorEnumData* data = (MonitorEnumData*)lParam;
            if (data->count < 16) {
                data->monitors[data->count] = hMon;
                data->count++;
            }
            return TRUE;
        }, (LPARAM)&enumData);
        
        actualCount = enumData.count;
        for (int i = 0; i < actualCount; i++) {
            monitors[i] = enumData.monitors[i];
            if (enumData.monitors[i] == hMonitor) {
                monitorIndex = i;
                // Получаем координаты центра монитора для надежной идентификации
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfo(hMonitor, &mi)) {
                    monitorCenterX = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
                    monitorCenterY = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
                }
            }
        }
    }
    
    // Сохраняем в INI файл
    char buffer[256];
    
    // Позиция и размер окна
    sprintf_s(buffer, "%d", windowX);
    WritePrivateProfileStringA("Window", "X", buffer, configPath.c_str());
    sprintf_s(buffer, "%d", windowY);
    WritePrivateProfileStringA("Window", "Y", buffer, configPath.c_str());
    sprintf_s(buffer, "%d", windowWidth);
    WritePrivateProfileStringA("Window", "Width", buffer, configPath.c_str());
    sprintf_s(buffer, "%d", windowHeight);
    WritePrivateProfileStringA("Window", "Height", buffer, configPath.c_str());
    
    // Сохраняем индекс монитора
    sprintf_s(buffer, "%d", monitorIndex);
    WritePrivateProfileStringA("Window", "Monitor", buffer, configPath.c_str());
    
    // Сохраняем координаты центра монитора для надежной идентификации
    sprintf_s(buffer, "%d", monitorCenterX);
    WritePrivateProfileStringA("Window", "MonitorCenterX", buffer, configPath.c_str());
    sprintf_s(buffer, "%d", monitorCenterY);
    WritePrivateProfileStringA("Window", "MonitorCenterY", buffer, configPath.c_str());
    
    // Видимость чата
    sprintf_s(buffer, "%d", g_content2Visible ? 1 : 0);
    WritePrivateProfileStringA("UI", "ChatVisible", buffer, configPath.c_str());
}

// Загрузка настроек из файла
void LoadSettings()
{
    if (!g_hWnd)
        return;
    
    // Получаем путь к файлу настроек
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string configPath = exePath;
    size_t lastSlash = configPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        configPath = configPath.substr(0, lastSlash + 1) + "config.ini";
    } else {
        configPath = "config.ini";
    }
    
    // Загружаем позицию и размер окна
    int windowX = GetPrivateProfileIntA("Window", "X", -1, configPath.c_str());
    int windowY = GetPrivateProfileIntA("Window", "Y", -1, configPath.c_str());
    int windowWidth = GetPrivateProfileIntA("Window", "Width", -1, configPath.c_str());
    int windowHeight = GetPrivateProfileIntA("Window", "Height", -1, configPath.c_str());
    int monitorIndex = GetPrivateProfileIntA("Window", "Monitor", -1, configPath.c_str());
    int savedMonitorCenterX = GetPrivateProfileIntA("Window", "MonitorCenterX", 0, configPath.c_str());
    int savedMonitorCenterY = GetPrivateProfileIntA("Window", "MonitorCenterY", 0, configPath.c_str());
    
    // Если настройки найдены, применяем их
    if (windowX != -1 && windowY != -1 && windowWidth > 0 && windowHeight > 0) {
        // Получаем информацию о мониторах
        struct MonitorEnumData {
            HMONITOR monitors[16];
            int count = 0;
        } enumData;
        
        int monitorCount = GetSystemMetrics(SM_CMONITORS);
        if (monitorCount <= 16) {
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                MonitorEnumData* data = (MonitorEnumData*)lParam;
                if (data->count < 16) {
                    data->monitors[data->count] = hMon;
                    data->count++;
                }
                return TRUE;
            }, (LPARAM)&enumData);
        }
        
        // Выбираем нужный монитор: сначала по координатам центра (надежнее), потом по индексу
        HMONITOR targetMonitor = nullptr;
        
        if (savedMonitorCenterX != 0 || savedMonitorCenterY != 0) {
            // Ищем монитор по сохраненным координатам центра (надежнее, чем по индексу)
            for (int i = 0; i < enumData.count; i++) {
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfo(enumData.monitors[i], &mi)) {
                    int centerX = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
                    int centerY = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
                    // Проверяем, совпадают ли координаты центра (с допуском в 10 пикселей)
                    if (abs(centerX - savedMonitorCenterX) < 10 && abs(centerY - savedMonitorCenterY) < 10) {
                        targetMonitor = enumData.monitors[i];
                        break;
                    }
                }
            }
        }
        
        // Если не нашли по координатам, используем индекс
        if (!targetMonitor && monitorIndex >= 0 && monitorIndex < enumData.count) {
            targetMonitor = enumData.monitors[monitorIndex];
        }
        
        // Если все еще не нашли, используем первый монитор
        if (!targetMonitor && enumData.count > 0) {
            targetMonitor = enumData.monitors[0];
        }
        
        // Проверяем, что окно находится на видимом мониторе
        MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
        if (targetMonitor && GetMonitorInfo(targetMonitor, &monitorInfo)) {
            // Проверяем, что позиция находится в пределах рабочей области монитора
            if (windowX >= monitorInfo.rcWork.left && windowX < monitorInfo.rcWork.right &&
                windowY >= monitorInfo.rcWork.top && windowY < monitorInfo.rcWork.bottom &&
                (windowX + windowWidth) <= monitorInfo.rcWork.right &&
                (windowY + windowHeight) <= monitorInfo.rcWork.bottom) {
                // Позиция валидна - устанавливаем окно
                SetWindowPos(g_hWnd, nullptr, windowX, windowY, windowWidth, windowHeight, 
                            SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                // Если позиция вне монитора, центрируем на нужном мониторе
                int centerX = monitorInfo.rcWork.left + (monitorInfo.rcWork.right - monitorInfo.rcWork.left) / 2 - windowWidth / 2;
                int centerY = monitorInfo.rcWork.top + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) / 2 - windowHeight / 2;
                SetWindowPos(g_hWnd, nullptr, centerX, centerY, windowWidth, windowHeight, 
                            SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else {
            // Если монитор не найден, проверяем, что позиция валидна на любом мониторе
            HMONITOR monitorAtPos = MonitorFromPoint(POINT{ windowX, windowY }, MONITOR_DEFAULTTONULL);
            if (monitorAtPos) {
                // Позиция валидна - устанавливаем окно
                SetWindowPos(g_hWnd, nullptr, windowX, windowY, windowWidth, windowHeight, 
                            SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                // Позиция невалидна - центрируем на основном мониторе
                HMONITOR primaryMonitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO primaryInfo = { sizeof(MONITORINFO) };
                if (GetMonitorInfo(primaryMonitor, &primaryInfo)) {
                    int centerX = primaryInfo.rcWork.left + (primaryInfo.rcWork.right - primaryInfo.rcWork.left) / 2 - windowWidth / 2;
                    int centerY = primaryInfo.rcWork.top + (primaryInfo.rcWork.bottom - primaryInfo.rcWork.top) / 2 - windowHeight / 2;
                    SetWindowPos(g_hWnd, nullptr, centerX, centerY, windowWidth, windowHeight, 
                                SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
    }
    
    // Загружаем видимость чата
    g_content2Visible = GetPrivateProfileIntA("UI", "ChatVisible", 0, configPath.c_str()) != 0;
}

