#include "OllamaOCR.hpp"
#include "Base64.hpp"
#include <iostream>

COllamaOCR::COllamaOCR() {
    m_sModel = "gemma3:12b"; // Default model as in Rust
}

size_t COllamaOCR::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<SOCRResult> COllamaOCR::recognize(const std::vector<unsigned char>& imageBytes) {
    std::vector<SOCRResult> results;
    
    CURL* curl = curl_easy_init();
    if (!curl) return results;

    std::string base64_image = base64_encode(imageBytes);
    
    Json::Value root;
    root["model"] = m_sModel;
    if (m_bTranslate)
        root["prompt"] = "Traduce el texto en la imagen al español, solo responde con la traducción";
    else
        root["prompt"] = "Extrae cualquier texto visible en esta imagen. Responde únicamente con el texto extraído.";
    root["stream"] = false;
    
    Json::Value images(Json::arrayValue);
    images.append(base64_image);
    root["images"] = images;

    Json::StreamWriterBuilder wbuilder;
    std::string requestBody = Json::writeString(wbuilder, root);

    std::string responseString;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    std::string url = "http://localhost:11434/api/generate";

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
                std::string extractedText = respJson["response"].asString();
                results.push_back({extractedText});
            } catch (...) {
                std::cerr << "Failed to parse Ollama response" << std::endl;
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return results;
}
