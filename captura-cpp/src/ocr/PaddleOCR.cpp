#include "PaddleOCR.hpp"
#include <arpa/inet.h>
#include <cstring>

std::vector<SOCRResult> CPaddleOCR::recognize(const std::vector<unsigned char>& imageBytes) {
    std::vector<SOCRResult> results;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return results;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/paddle_socket_unix", sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return results;
    }

    // Send image size (u64, big-endian)
    uint64_t size = htobe64(imageBytes.size());
    if (send(sock, &size, sizeof(size), 0) < 0) {
        perror("send size");
        close(sock);
        return results;
    }

    // Send image data
    if (send(sock, imageBytes.data(), imageBytes.size(), 0) < 0) {
        perror("send data");
        close(sock);
        return results;
    }

    // Read response size (u64, big-endian)
    uint64_t respSizeBE;
    if (recv(sock, &respSizeBE, sizeof(respSizeBE), 0) < sizeof(respSizeBE)) {
        perror("recv resp size");
        close(sock);
        return results;
    }
    uint64_t respSize = be64toh(respSizeBE);

    if (respSize == 0) {
        close(sock);
        return results;
    }

    std::string respBody;
    respBody.resize(respSize);
    size_t received = 0;
    while (received < respSize) {
        ssize_t r = recv(sock, &respBody[received], respSize - received, 0);
        if (r <= 0) break;
        received += r;
    }

    close(sock);

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(respBody.c_str(), respBody.c_str() + respBody.size(), &root, &errs)) {
        std::cerr << "Json parse error: " << errs << std::endl;
        return results;
    }

    Json::Value resObj = root;
    if (root.isMember("res")) {
        resObj = root["res"];
    }

    if (resObj.isMember("rec_texts") && resObj.isMember("dt_polys")) {
        const auto& texts = resObj["rec_texts"];
        const auto& polys = resObj["dt_polys"];
        for (Json::Value::ArrayIndex i = 0; i < texts.size() && i < polys.size(); ++i) {
            // dt_polys is usually [[x1,y1],[x2,y2],[x3,y3],[x4,y4]]
            const auto& poly = polys[i];
            if (poly.size() >= 4) {
                float minX = poly[0][0].asFloat();
                float minY = poly[0][1].asFloat();
                float maxX = minX;
                float maxY = minY;
                for (int j = 1; j < 4; ++j) {
                    minX = std::min(minX, poly[j][0].asFloat());
                    minY = std::min(minY, poly[j][1].asFloat());
                    maxX = std::max(maxX, poly[j][0].asFloat());
                    maxY = std::max(maxY, poly[j][1].asFloat());
                }
                results.push_back({
                    texts[i].asString(),
                    Vector2D(minX, minY),
                    Vector2D(maxX - minX, maxY - minY)
                });
            } else {
                results.push_back({texts[i].asString(), Vector2D(0,0), Vector2D(0,0)});
            }
        }
    } else if (resObj.isMember("rec_texts")) {
        for (const auto& text : resObj["rec_texts"]) {
            results.push_back({text.asString(), Vector2D(0,0), Vector2D(0,0)});
        }
    }

    return results;
}
