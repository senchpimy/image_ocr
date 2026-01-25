#include <cstdint>
#include <format>
#include <regex>
#include <strings.h>
#include <getopt.h>

#include <iostream>

#include "captura.hpp"

#ifdef HAS_PADDLE
#include "ocr/PaddleOCR.hpp"
#endif
#ifdef HAS_TESSERACT
#include "ocr/TesseractOCR.hpp"
#endif
#ifdef HAS_GEMINI
#include "ocr/GeminiOCR.hpp"
#endif
#ifdef HAS_OLLAMA
#include "ocr/OllamaOCR.hpp"
#endif

static void help() {
    std::cout << "Captura-CPP usage: captura-cpp [arg [...]].\n\nArguments:\n"
              << " -a | --autocopy            | Automatically copies the output to the clipboard (requires wl-clipboard)\n"
              << " -n | --notify              | Sends a desktop notification when text is recognized\n"
              << " -h | --help                | Show this help message\n"
              << " -r | --render-inactive     | Render (freeze) inactive displays\n"
              << " -q | --quiet               | Disable most logs (leaves errors)\n"
              << " -v | --verbose             | Enable more logs\n"
              << " -t | --no-fractional       | Disable fractional scaling support\n"
              << " -P | --provider=name       | Set OCR provider ("
#ifdef HAS_PADDLE
              << "paddle, "
#endif
#ifdef HAS_TESSERACT
              << "tesseract, "
#endif
#ifdef HAS_GEMINI
              << "gemini, "
#endif
#ifdef HAS_OLLAMA
              << "ollama"
#endif
              << ")\n"
              << " -L | --lang=lang           | Set language for Tesseract (default: spa)\n"
              << " -T | --translate           | Translate the text to Spanish (only for AI providers)\n"
              << " -V | --version             | Print version info\n";
}

int main(int argc, char** argv, char** envp) {
    g_pCaptura = std::make_unique<CCaptura>();

    while (true) {
        int                  option_index   = 0;
        static struct option long_options[] = {{"autocopy", no_argument, nullptr, 'a'},
                                               {"help", no_argument, nullptr, 'h'},
                                               {"notify", no_argument, nullptr, 'n'},
                                               {"render-inactive", no_argument, nullptr, 'r'},
                                               {"no-fractional", no_argument, nullptr, 't'},
                                               {"quiet", no_argument, nullptr, 'q'},
                                               {"verbose", no_argument, nullptr, 'v'},
                                               {"version", no_argument, nullptr, 'V'},
                                               {"provider", required_argument, nullptr, 'P'},
                                               {"lang", required_argument, nullptr, 'L'},
                                               {"translate", no_argument, nullptr, 'T'},
                                               {nullptr, 0, nullptr, 0}};

        int                  c = getopt_long(argc, argv, ":hnartzqvV P:L:T", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'L': g_pCaptura->m_sLang = optarg; break;
            case 'P':
#ifdef HAS_PADDLE
                if (strcasecmp(optarg, "paddle") == 0)
                    g_pCaptura->m_pOCR = std::make_unique<CPaddleOCR>();
                else
#endif
#ifdef HAS_TESSERACT
                if (strcasecmp(optarg, "tesseract") == 0)
                    g_pCaptura->m_pOCR = std::make_unique<CTesseractOCR>();
                else
#endif
#ifdef HAS_GEMINI
                if (strcasecmp(optarg, "gemini") == 0)
                    g_pCaptura->m_pOCR = std::make_unique<CGeminiOCR>();
                else
#endif
#ifdef HAS_OLLAMA
                if (strcasecmp(optarg, "ollama") == 0)
                    g_pCaptura->m_pOCR = std::make_unique<COllamaOCR>();
                else
#endif
                {
                    Debug::log(NONE, "Unrecognized or disabled OCR provider %s", optarg);
                    exit(1);
                }
                break;
            case 'h': help(); exit(0);
            case 'n': g_pCaptura->m_bNotify = true; break;
            case 'a': g_pCaptura->m_bAutoCopy = true; break;
            case 'r': g_pCaptura->m_bRenderInactive = true; break;
            case 't': g_pCaptura->m_bNoFractional = true; break;
            case 'q': Debug::quiet = true; break;
            case 'v': Debug::verbose = true; break;
            case 'V': {
                std::cout << "captura-cpp v" << CAPTURA_VERSION << "\n";
                exit(0);
            }
            case 'T': g_pCaptura->m_bTranslate = true; break;
            default: help(); exit(1);
        }
    }

    if (!isatty(fileno(stdout)) || getenv("NO_COLOR"))
        Debug::log(TRACE, "No color output");

    g_pCaptura->init();

    return 0;
}
