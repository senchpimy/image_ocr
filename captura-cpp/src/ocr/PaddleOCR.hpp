#pragma once
#include "OCR.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <json/json.h>

class CPaddleOCR : public IOCRProvider {
public:
    std::string name() override { return "PaddleOCR"; }
    std::vector<SOCRResult> recognize(const std::vector<unsigned char>& imageBytes) override;
};
