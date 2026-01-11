// FontEmbedded.h - Загрузка шрифта из Windows Resources
// Шрифт symbols_skyquake.ttf встраивается в exe через font_resource.rc

#ifndef FONT_EMBEDDED_H
#define FONT_EMBEDDED_H

#include <windows.h>
#include <cstddef>

// Класс для работы с встроенным шрифтом
class FontData {
public:
    // Получить указатель на данные шрифта
    static const unsigned char* GetData() {
        static const unsigned char* data = nullptr;
        static bool initialized = false;
        
        if (!initialized) {
            // Загружаем ресурс из exe
            // RT_RCDATA = 10, используем числовое значение напрямую для избежания проблем с Unicode/ANSI
            HRSRC hResource = FindResourceA(nullptr, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(10));
            if (hResource != nullptr) {
                HGLOBAL hMemory = LoadResource(nullptr, hResource);
                if (hMemory != nullptr) {
                    data = static_cast<const unsigned char*>(LockResource(hMemory));
                    initialized = true;
                }
            }
        }
        
        return data;
    }
    
    // Получить размер данных шрифта
    static size_t GetSize() {
        static size_t size = 0;
        static bool initialized = false;
        
        if (!initialized) {
            // RT_RCDATA = 10, используем числовое значение напрямую
            HRSRC hResource = FindResourceA(nullptr, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(10));
            if (hResource != nullptr) {
                size = SizeofResource(nullptr, hResource);
                initialized = true;
            }
        }
        
        return size;
    }
};

#endif // FONT_EMBEDDED_H
