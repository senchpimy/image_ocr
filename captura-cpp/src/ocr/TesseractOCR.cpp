#include "TesseractOCR.hpp"
#include <iostream>

CTesseractOCR::CTesseractOCR() {
    m_pApi = new tesseract::TessBaseAPI();
}

CTesseractOCR::~CTesseractOCR() {
    m_pApi->End();
    delete m_pApi;
}

std::vector<SOCRResult> CTesseractOCR::recognize(const std::vector<unsigned char>& imageBytes) {
    std::vector<SOCRResult> results;

    if (m_pApi->Init(NULL, m_sLang.c_str())) {
        fprintf(stderr, "Could not initialize tesseract with lang %s.\n", m_sLang.c_str());
        return results;
    }
    
    Pix* image = pixReadMem(imageBytes.data(), imageBytes.size());
    if (!image) {
        return results;
    }

    m_pApi->SetImage(image);
    m_pApi->Recognize(0);
    tesseract::ResultIterator* ri = m_pApi->GetIterator();
    tesseract::PageIteratorLevel level = tesseract::RIL_WORD;

    if (ri) {
        do {
            const char* word = ri->GetUTF8Text(level);
            if (!word) continue;
            
            int x1, y1, x2, y2;
            ri->BoundingBox(level, &x1, &y1, &x2, &y2);
            
            results.push_back({
                std::string(word),
                Vector2D(x1, y1),
                Vector2D(x2 - x1, y2 - y1)
            });
            
            delete[] word;
        } while (ri->Next(level));
        delete ri;
    }

    pixDestroy(&image);
    return results;
}

