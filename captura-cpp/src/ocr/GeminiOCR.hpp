#pragma once
#include "OCR.hpp"
#include <curl/curl.h>
#include <json/json.h>

class CGeminiOCR : public IOCRProvider {
public:
    CGeminiOCR();
    std::string name() override { return "Gemini"; }
    std::vector<SOCRResult> recognize(const std::vector<unsigned char>& imageBytes) override;
    void setTranslate(bool translate) override { m_bTranslate = translate; }

private:
    std::string m_sApiKey;
    bool m_bTranslate = false;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
};
