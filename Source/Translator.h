#pragma once
#include <string>
#include <unordered_map>

class Translator {
public:
    Translator();
    const std::string& Get(const std::string& key) const;
    void SetLang(const std::string& lang);

private:
    std::string currentLang;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> translations;
};

// Accesseur global
Translator& TR();