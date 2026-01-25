#pragma once
#include "OCR.hpp"
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

class CTesseractOCR : public IOCRProvider {
public:
    CTesseractOCR();
    ~CTesseractOCR();
    std::string name() override { return "Tesseract"; }
    std::vector<SOCRResult> recognize(const std::vector<unsigned char>& imageBytes) override;
    void setLang(const std::string& lang) override { m_sLang = lang; }

private:
    tesseract::TessBaseAPI* m_pApi;
    std::string m_sLang = "spa";
};
