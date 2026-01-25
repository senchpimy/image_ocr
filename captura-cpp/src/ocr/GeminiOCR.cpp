#include "GeminiOCR.hpp"
#include "Base64.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

CGeminiOCR::CGeminiOCR() {
    char* envKey = getenv("GEMINI_API_KEY");
    if (envKey) {
        m_sApiKey = envKey;
    } else {
        std::vector<std::string> paths = {"gemini", "../gemini", "../../gemini"};
        for (const auto& path : paths) {
            std::ifstream ifs(path);
            if (ifs.is_open()) {
                std::getline(ifs, m_sApiKey);
                if (!m_sApiKey.empty()) break;
            }
        }
    }
}

size_t CGeminiOCR::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<SOCRResult> CGeminiOCR::recognize(const std::vector<unsigned char>& imageBytes) {
    std::vector<SOCRResult> results;
    if (m_sApiKey.empty()) {
        std::cerr << "Gemini API key missing" << std::endl;
        return results;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return results;

    std::string base64_image = base64_encode(imageBytes);
    
    Json::Value root;
    Json::Value contents(Json::arrayValue);
    Json::Value content;
    Json::Value parts(Json::arrayValue);
    
    Json::Value partText;
    if (m_bTranslate)
        partText["text"] = "Traduce el texto en la imagen al español, solo responde con la traducción";
    else
        partText["text"] = "Extrae cualquier texto visible en esta imagen. Responde únicamente con el texto extraído.";
    parts.append(partText);
    
    Json::Value partImage;
    partImage["inline_data"]["mime_type"] = "image/png";
    partImage["inline_data"]["data"] = base64_image;
    parts.append(partImage);
    
    content["parts"] = parts;
    contents.append(content);
    root["contents"] = contents;

    Json::StreamWriterBuilder wbuilder;
    std::string requestBody = Json::writeString(wbuilder, root);

    std::string responseString;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-lite:generateContent?key=" + m_sApiKey;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    } else {
        Json::Value respJson;
        Json::CharReaderBuilder rbuilder;
        std::string errs;
        std::unique_ptr<Json::CharReader> reader(rbuilder.newCharReader());
        if (reader->parse(responseString.c_str(), responseString.c_str() + responseString.size(), &respJson, &errs)) {
            try {
                std::string extractedText = respJson["candidates"][0]["content"]["parts"][0]["text"].asString();
                results.push_back({extractedText});
            } catch (...) {
                std::cerr << "Failed to parse Gemini response" << std::endl;
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return results;
}
