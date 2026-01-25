#pragma once
#include <string>
#include <vector>
#include <memory>

#include <hyprutils/math/Vector2D.hpp>
using namespace Hyprutils::Math;

struct SOCRResult {
    std::string text;
    Vector2D pos;
    Vector2D size;
};

class IOCRProvider {
public:
    virtual ~IOCRProvider() = default;
    virtual std::string name() = 0;
    virtual std::vector<SOCRResult> recognize(const std::vector<unsigned char>& imageBytes) = 0;
    virtual void setTranslate(bool translate) {}
    virtual void setLang(const std::string& lang) {}
};
