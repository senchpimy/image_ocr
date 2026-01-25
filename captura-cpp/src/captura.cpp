#include "captura.hpp"
#include "src/debug/Log.hpp"
#include "src/notify/Notify.hpp"
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
#include "src/clipboard/Clipboard.hpp"
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <format>
#include <hyprutils/math/Vector2D.hpp>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <pango/pangocairo.h>

static void sigHandler(int sig) {
    g_pCaptura->m_vLayerSurfaces.clear();
    exit(0);
}

void CCaptura::init() {
    m_pXKBContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_pXKBContext)
        Debug::log(ERR, "Failed to create xkb context, keyboard movement not supported");

    m_pWLDisplay = wl_display_connect(nullptr);

    if (!m_pWLDisplay) {
        Debug::log(CRIT, "No wayland compositor running!");
        exit(1);
        return;
    }

    signal(SIGTERM, sigHandler);

    m_pRegistry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(m_pWLDisplay));
    m_pRegistry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        if (strcmp(interface, wl_compositor_interface.name) == 0) {
            m_pCompositor = makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_compositor_interface, 4));
        } else if (strcmp(interface, wl_shm_interface.name) == 0) {
            m_pSHM = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_shm_interface, 1));
        } else if (strcmp(interface, wl_output_interface.name) == 0) {
            m_mtTickMutex.lock();

            const auto PMONITOR = g_pCaptura->m_vMonitors
                                      .emplace_back(std::make_unique<SMonitor>(
                                          makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_output_interface, 4))))
                                      .get();
            PMONITOR->wayland_name = name;

            m_mtTickMutex.unlock();
        } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
            m_pLayerShell = makeShared<CCZwlrLayerShellV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &zwlr_layer_shell_v1_interface, 1));
        } else if (strcmp(interface, wl_seat_interface.name) == 0) {
            m_pSeat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_seat_interface, 1));

            m_pSeat->setCapabilities([this](CCWlSeat* seat, uint32_t caps) {
                if (caps & WL_SEAT_CAPABILITY_POINTER) {
                    if (!m_pPointer) {
                        m_pPointer = makeShared<CCWlPointer>(m_pSeat->sendGetPointer());
                        initMouse();
                        if (m_pCursorShapeMgr)
                            m_pCursorShapeDevice = makeShared<CCWpCursorShapeDeviceV1>(m_pCursorShapeMgr->sendGetPointer(m_pPointer->resource()));
                    }
                } else {
                    Debug::log(CRIT, "Captura cannot work without a pointer!");
                    g_pCaptura->finish(1);
                }

                if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
                    if (!m_pKeyboard) {
                        m_pKeyboard = makeShared<CCWlKeyboard>(m_pSeat->sendGetKeyboard());
                        initKeyboard();
                    }
                } else
                    m_pKeyboard.reset();
            });

        } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
            m_pScreencopyMgr =
                makeShared<CCZwlrScreencopyManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &zwlr_screencopy_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
            m_pCursorShapeMgr =
                makeShared<CCWpCursorShapeManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_cursor_shape_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
            m_pFractionalMgr =
                makeShared<CCWpFractionalScaleManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_fractional_scale_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
            m_pViewporter = makeShared<CCWpViewporter>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_viewporter_interface, 1));
        }
    });

    wl_display_roundtrip(m_pWLDisplay);

    if (!m_pCursorShapeMgr)
        Debug::log(ERR, "cursor_shape_v1 not supported, cursor won't be affected");

    if (!m_pScreencopyMgr) {
        Debug::log(CRIT, "zwlr_screencopy_v1 not supported, can't proceed");
        exit(1);
    }

    if (!m_pFractionalMgr) {
        Debug::log(WARN, "wp_fractional_scale_v1 not supported, fractional scaling won't work");
        m_bNoFractional = true;
    }
    if (!m_pViewporter) {
        Debug::log(WARN, "wp_viewporter not supported, fractional scaling won't work");
        m_bNoFractional = true;
    }

    for (auto& m : m_vMonitors) {
        m_vLayerSurfaces.emplace_back(std::make_unique<CLayerSurface>(m.get()));

        m_pLastSurface = m_vLayerSurfaces.back().get();

        m->pSCFrame = makeShared<CCZwlrScreencopyFrameV1>(m_pScreencopyMgr->sendCaptureOutput(false, m->output->resource()));
        m->pLS      = m_vLayerSurfaces.back().get();
        m->initSCFrame();
    }

    wl_display_roundtrip(m_pWLDisplay);

    while (m_bRunning && wl_display_dispatch(m_pWLDisplay) != -1) {
        //renderSurface(m_pLastSurface);
    }

    if (m_pWLDisplay) {
        wl_display_disconnect(m_pWLDisplay);
        m_pWLDisplay = nullptr;
    }
}

void CCaptura::finish(int code) {
    m_vLayerSurfaces.clear();

    if (m_pWLDisplay) {
        m_vLayerSurfaces.clear();
        m_vMonitors.clear();
        m_pCompositor.reset();
        m_pRegistry.reset();
        m_pSHM.reset();
        m_pLayerShell.reset();
        m_pScreencopyMgr.reset();
        m_pCursorShapeMgr.reset();
        m_pCursorShapeDevice.reset();
        m_pSeat.reset();
        m_pKeyboard.reset();
        m_pPointer.reset();
        m_pViewporter.reset();
        m_pFractionalMgr.reset();

        wl_display_disconnect(m_pWLDisplay);
        m_pWLDisplay = nullptr;
    }

    exit(code);
}

void CCaptura::recheckACK() {
    for (auto& ls : m_vLayerSurfaces) {
        if ((ls->wantsACK || ls->wantsReload) && ls->screenBuffer) {
            if (ls->wantsACK)
                ls->pLayerSurface->sendAckConfigure(ls->ACKSerial);
            ls->wantsACK    = false;
            ls->wantsReload = false;

            const auto MONITORSIZE =
                (ls->screenBuffer && !g_pCaptura->m_bNoFractional ? ls->m_pMonitor->size * ls->fractionalScale : ls->m_pMonitor->size * ls->m_pMonitor->scale).round();

            if (!ls->buffers[0] || ls->buffers[0]->pixelSize != MONITORSIZE) {
                Debug::log(TRACE, "making new buffers: size changed to %.0fx%.0f", MONITORSIZE.x, MONITORSIZE.y);
                ls->buffers[0] = makeShared<SPoolBuffer>(MONITORSIZE, WL_SHM_FORMAT_ARGB8888, MONITORSIZE.x * 4);
                ls->buffers[1] = makeShared<SPoolBuffer>(MONITORSIZE, WL_SHM_FORMAT_ARGB8888, MONITORSIZE.x * 4);
            }
        }
    }

    markDirty();
}

void CCaptura::markDirty() {
    for (auto& ls : m_vLayerSurfaces) {
        if (ls->frameCallback)
            continue;

        ls->markDirty();
    }
}

SP<SPoolBuffer> CCaptura::getBufferForLS(CLayerSurface* pLS) {
    SP<SPoolBuffer> returns = nullptr;

    for (auto i = 0; i < 2; ++i) {
        if (!pLS->buffers[i] || pLS->buffers[i]->busy)
            continue;

        returns = pLS->buffers[i];
    }

    return returns;
}

bool CCaptura::setCloexec(const int& FD) {
    long flags = fcntl(FD, F_GETFD);
    if (flags == -1) {
        return false;
    }

    if (fcntl(FD, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return false;
    }

    return true;
}

int CCaptura::createPoolFile(size_t size, std::string& name) {
    const auto XDGRUNTIMEDIR = getenv("XDG_RUNTIME_DIR");
    if (!XDGRUNTIMEDIR) {
        Debug::log(CRIT, "XDG_RUNTIME_DIR not set!");
        g_pCaptura->finish(1);
    }

    name = std::string(XDGRUNTIMEDIR) + "/.captura_XXXXXX";

    const auto FD = mkstemp((char*)name.c_str());
    if (FD < 0) {
        Debug::log(CRIT, "createPoolFile: fd < 0");
        g_pCaptura->finish(1);
    }

    if (!setCloexec(FD)) {
        close(FD);
        Debug::log(CRIT, "createPoolFile: !setCloexec");
        g_pCaptura->finish(1);
    }

    if (ftruncate(FD, size) < 0) {
        close(FD);
        Debug::log(CRIT, "createPoolFile: ftruncate < 0");
        g_pCaptura->finish(1);
    }

    return FD;
}

void CCaptura::convertBuffer(SP<SPoolBuffer> pBuffer) {
    switch (pBuffer->format) {
        case WL_SHM_FORMAT_ARGB8888:
        case WL_SHM_FORMAT_XRGB8888: break;
        case WL_SHM_FORMAT_ABGR8888:
        case WL_SHM_FORMAT_XBGR8888: {
            uint8_t* data = (uint8_t*)pBuffer->data;

            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel {
                        // little-endian ARGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                        unsigned char alpha;
                    }* px = (struct SPixel*)(data + (static_cast<ptrdiff_t>(y * (int)pBuffer->pixelSize.x * 4)) + (static_cast<ptrdiff_t>(x * 4)));

                    std::swap(px->red, px->blue);
                }
            }
        } break;
        case WL_SHM_FORMAT_XRGB2101010:
        case WL_SHM_FORMAT_XBGR2101010: {
            uint8_t*   data = (uint8_t*)pBuffer->data;

            const bool FLIP = pBuffer->format == WL_SHM_FORMAT_XBGR2101010;

            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    uint32_t* px = (uint32_t*)(data + (static_cast<ptrdiff_t>(y * (int)pBuffer->pixelSize.x * 4)) + (static_cast<ptrdiff_t>(x * 4)));

                    // conv to 8 bit
                    uint8_t R = (uint8_t)std::round((255.0 * (((*px) & 0b00000000000000000000001111111111) >> 0) / 1023.0));
                    uint8_t G = (uint8_t)std::round((255.0 * (((*px) & 0b00000000000011111111110000000000) >> 10) / 1023.0));
                    uint8_t B = (uint8_t)std::round((255.0 * (((*px) & 0b00111111111100000000000000000000) >> 20) / 1023.0));
                    uint8_t A = (uint8_t)std::round((255.0 * (((*px) & 0b11000000000000000000000000000000) >> 30) / 3.0));

                    // write 8-bit values
                    *px = ((FLIP ? B : R) << 0) + (G << 8) + ((FLIP ? R : B) << 16) + (A << 24);
                }
            }
        } break;
        default: {
            Debug::log(CRIT, "Unsupported format %i", pBuffer->format);
        }
            g_pCaptura->finish(1);
    }
}

// Mallocs a new buffer, which needs to be free'd!
void* CCaptura::convert24To32Buffer(SP<SPoolBuffer> pBuffer) {
    uint8_t* newBuffer       = (uint8_t*)malloc((size_t)pBuffer->pixelSize.x * pBuffer->pixelSize.y * 4);
    int      newBufferStride = pBuffer->pixelSize.x * 4;
    uint8_t* oldBuffer       = (uint8_t*)pBuffer->data;

    switch (pBuffer->format) {
        case WL_SHM_FORMAT_BGR888: {
            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel3 {
                        // little-endian RGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                    }* srcPx = (struct SPixel3*)(oldBuffer + (static_cast<size_t>(y * pBuffer->stride)) + (static_cast<ptrdiff_t>(x * 3)));
                    struct SPixel4 {
                        // little-endian ARGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                        unsigned char alpha;
                    }* dstPx = (struct SPixel4*)(newBuffer + (static_cast<ptrdiff_t>(y * newBufferStride)) + (static_cast<ptrdiff_t>(x * 4)));
                    *dstPx   = {.blue = srcPx->red, .green = srcPx->green, .red = srcPx->blue, .alpha = 0xFF};
                }
            }
        } break;
        case WL_SHM_FORMAT_RGB888: {
            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel3 {
                        // big-endian RGB
                        unsigned char red;
                        unsigned char green;
                        unsigned char blue;
                    }* srcPx = (struct SPixel3*)(oldBuffer + (y * pBuffer->stride) + (x * 3));
                    struct SPixel4 {
                        // big-endian ARGB
                        unsigned char alpha;
                        unsigned char red;
                        unsigned char green;
                        unsigned char blue;
                    }* dstPx = (struct SPixel4*)(newBuffer + (y * newBufferStride) + (x * 4));
                    *dstPx   = {.alpha = 0xFF, .red = srcPx->red, .green = srcPx->green, .blue = srcPx->blue};
                }
            }
        } break;
        default: {
            Debug::log(CRIT, "Unsupported format for 24bit buffer %i", pBuffer->format);
        }
            g_pCaptura->finish(1);
    }
    return newBuffer;
}

static cairo_status_t write_png_to_vec(void* closure, const unsigned char* data, unsigned int length) {
    auto* vec = (std::vector<unsigned char>*)closure;
    vec->insert(vec->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

void CCaptura::finishSelection() {
    // Determine selection bounds
    double minX = std::min(m_vSelectionStart.x, m_vSelectionEnd.x);
    double minY = std::min(m_vSelectionStart.y, m_vSelectionEnd.y);
    double maxX = std::max(m_vSelectionStart.x, m_vSelectionEnd.x);
    double maxY = std::max(m_vSelectionStart.y, m_vSelectionEnd.y);

    if (maxX - minX < 5 || maxY - minY < 5) {
        return;
    }

    if (!m_pLastSurface || !m_pLastSurface->screenBuffer) {
        finish(1);
        return;
    }

    const auto SCALEBUFS = m_pLastSurface->screenBuffer->pixelSize / m_pLastSurface->m_pMonitor->size;
    
    int cropX = (int)(minX * SCALEBUFS.x);
    int cropY = (int)(minY * SCALEBUFS.y);
    int cropW = (int)((maxX - minX) * SCALEBUFS.x);
    int cropH = (int)((maxY - minY) * SCALEBUFS.y);

    // Create a new surface for the cropped image
    cairo_surface_t* cropSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cropW, cropH);
    cairo_t* cr = cairo_create(cropSurface);

    cairo_set_source_surface(cr, m_pLastSurface->screenBuffer->surface, -cropX, -cropY);
    cairo_rectangle(cr, 0, 0, cropW, cropH);
    cairo_fill(cr);

    m_vLastSelectionPng.clear();
    cairo_surface_write_to_png_stream(cropSurface, write_png_to_vec, &m_vLastSelectionPng);

    cairo_destroy(cr);
    cairo_surface_destroy(cropSurface);

    m_vLastSelectionMin = Vector2D(minX, minY);
    m_vLastResults.clear();

    m_bMenuVisible = true;
    m_vMenuPos = m_vLastCoords;
    
    // Adjust menu pos to be within screen
    if (m_vMenuPos.x + 200 > m_pLastSurface->m_pMonitor->size.x) m_vMenuPos.x -= 200;
    if (m_vMenuPos.y + 300 > m_pLastSurface->m_pMonitor->size.y) m_vMenuPos.y -= 300;

    m_sResultText = "Selección lista. Elija una opción.";
    markDirty();
}

void CCaptura::renderSurface(CLayerSurface* pSurface, bool forceInactive) {
    const auto PBUFFER = getBufferForLS(pSurface);

    if (!PBUFFER || !pSurface->screenBuffer) {
        return;
    }

    PBUFFER->surface =
        cairo_image_surface_create_for_data((unsigned char*)PBUFFER->data, CAIRO_FORMAT_ARGB32, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y, PBUFFER->pixelSize.x * 4);

    PBUFFER->cairo = cairo_create(PBUFFER->surface);

    const auto PCAIRO = PBUFFER->cairo;

    cairo_save(PCAIRO);

    // Clear buffer
    cairo_set_source_rgba(PCAIRO, 0, 0, 0, 0);
    cairo_set_operator(PCAIRO, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(PCAIRO, 0, 0, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y);
    cairo_fill(PCAIRO);
    cairo_set_operator(PCAIRO, CAIRO_OPERATOR_OVER);

    if (pSurface == m_pLastSurface && !forceInactive && m_bCoordsInitialized) {
        const auto SCALEBUFS      = pSurface->screenBuffer->pixelSize / PBUFFER->pixelSize;
        
        // Draw the frozen screen content
        const auto PATTERNPRE = cairo_pattern_create_for_surface(pSurface->screenBuffer->surface);
        cairo_pattern_set_filter(PATTERNPRE, CAIRO_FILTER_BILINEAR);
        cairo_matrix_t matrixPre;
        cairo_matrix_init_identity(&matrixPre);
        cairo_matrix_scale(&matrixPre, SCALEBUFS.x, SCALEBUFS.y);
        cairo_pattern_set_matrix(PATTERNPRE, &matrixPre);
        cairo_set_source(PCAIRO, PATTERNPRE);
        cairo_paint(PCAIRO);
        cairo_pattern_destroy(PATTERNPRE);

        // Draw selection rectangle if selecting
        if (m_bIsSelecting || (m_vSelectionStart != m_vSelectionEnd)) {
            double minX = std::min(m_vSelectionStart.x, m_vSelectionEnd.x);
            double minY = std::min(m_vSelectionStart.y, m_vSelectionEnd.y);
            double maxX = std::max(m_vSelectionStart.x, m_vSelectionEnd.x);
            double maxY = std::max(m_vSelectionStart.y, m_vSelectionEnd.y);

            // Darken outside selection
            cairo_set_source_rgba(PCAIRO, 0, 0, 0, 0.3);
            
            // Top
            cairo_rectangle(PCAIRO, 0, 0, PBUFFER->pixelSize.x, minY);
            // Bottom
            cairo_rectangle(PCAIRO, 0, maxY, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y - maxY);
            // Left
            cairo_rectangle(PCAIRO, 0, minY, minX, maxY - minY);
            // Right
            cairo_rectangle(PCAIRO, maxX, minY, PBUFFER->pixelSize.x - maxX, maxY - minY);
            
            cairo_fill(PCAIRO);

            // Selection border
            cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 1.0);
            cairo_set_line_width(PCAIRO, 1.0);
            cairo_rectangle(PCAIRO, minX, minY, maxX - minX, maxY - minY);
            cairo_stroke(PCAIRO);

            // Draw handles
            cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 0.8);
            auto drawHandle = [&](double x, double y) {
                cairo_arc(PCAIRO, x, y, 4, 0, 2 * M_PI);
                cairo_fill(PCAIRO);
            };
            drawHandle(minX, minY);
            drawHandle(maxX, minY);
            drawHandle(minX, maxY);
            drawHandle(maxX, maxY);
        }

        if (m_bMenuVisible) {
            drawMenu(PCAIRO, m_vMenuPos);
            
            // Draw word markers
            const auto SCALEBUFS = pSurface->screenBuffer->pixelSize / pSurface->m_pMonitor->size;
            cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 0.2);
            for (const auto& res : m_vLastResults) {
                if (res.size.x > 0 && res.size.y > 0) {
                    double x = m_vLastSelectionMin.x + res.pos.x / SCALEBUFS.x;
                    double y = m_vLastSelectionMin.y + res.pos.y / SCALEBUFS.y;
                    double w = res.size.x / SCALEBUFS.x;
                    double h = res.size.y / SCALEBUFS.y;
                    
                    cairo_rectangle(PCAIRO, x, y, w, h);
                    cairo_fill(PCAIRO);
                    
                    cairo_save(PCAIRO);
                    cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 0.5);
                    cairo_set_line_width(PCAIRO, 1.0);
                    cairo_rectangle(PCAIRO, x, y, w, h);
                    cairo_stroke(PCAIRO);
                    cairo_restore(PCAIRO);
                }
            }
        }

        cairo_surface_flush(PBUFFER->surface);
    } else if (m_bCoordsInitialized) {
        // Just draw the screen content for other monitors
        const auto SCALEBUFS  = pSurface->screenBuffer->pixelSize / PBUFFER->pixelSize;
        const auto PATTERNPRE = cairo_pattern_create_for_surface(pSurface->screenBuffer->surface);
        cairo_pattern_set_filter(PATTERNPRE, CAIRO_FILTER_BILINEAR);
        cairo_matrix_t matrixPre;
        cairo_matrix_init_identity(&matrixPre);
        cairo_matrix_scale(&matrixPre, SCALEBUFS.x, SCALEBUFS.y);
        cairo_pattern_set_matrix(PATTERNPRE, &matrixPre);
        cairo_set_source(PCAIRO, PATTERNPRE);
        cairo_paint(PCAIRO);

        cairo_surface_flush(PBUFFER->surface);
        cairo_pattern_destroy(PATTERNPRE);
    }

    pSurface->sendFrame();
    cairo_destroy(PCAIRO);
    cairo_surface_destroy(PBUFFER->surface);

    PBUFFER->busy    = true;
    PBUFFER->cairo   = nullptr;
    PBUFFER->surface = nullptr;

    pSurface->rendered = true;
}

static void drawRoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void CCaptura::drawMenu(cairo_t* cr, Vector2D pos) {
    const int width = 340;
    const int buttonHeight = 36;
    const int padding = 15;
    const int borderRadius = 12;
    const int gap = 8;
    
    m_vMenuButtons.clear();

    // Setup buttons data (without emojis)
    struct ButtonDef { std::string label; std::function<void()> action; };
    std::vector<ButtonDef> defs;
    
#ifdef HAS_TESSERACT
    defs.push_back({"Tesseract", [this]() {
        m_sResultText = "Procesando con Tesseract...";
        markDirty();
        auto provider = std::make_unique<CTesseractOCR>();
        provider->setLang(m_sLang);
        m_vLastResults = provider->recognize(m_vLastSelectionPng);
        m_sResultText = "";
        for (const auto& r : m_vLastResults) m_sResultText += r.text + " ";
        markDirty();
    }});
#endif

#ifdef HAS_PADDLE
    defs.push_back({"PaddleOCR", [this]() {
        m_sResultText = "Procesando con PaddleOCR...";
        markDirty();
        auto provider = std::make_unique<CPaddleOCR>();
        m_vLastResults = provider->recognize(m_vLastSelectionPng);
        m_sResultText = "";
        for (const auto& r : m_vLastResults) m_sResultText += r.text + " ";
        markDirty();
    }});
#endif

#ifdef HAS_OLLAMA
    defs.push_back({"Ollama AI", [this]() {
        m_sResultText = "Consultando a Ollama...";
        markDirty();
        auto provider = std::make_unique<COllamaOCR>();
        provider->setTranslate(m_bTranslate);
        m_vLastResults = provider->recognize(m_vLastSelectionPng);
        m_sResultText = "";
        for (const auto& r : m_vLastResults) m_sResultText += r.text + " ";
        markDirty();
    }});
#endif

#ifdef HAS_GEMINI
    defs.push_back({"Gemini AI", [this]() {
        m_sResultText = "Consultando a Gemini...";
        markDirty();
        auto provider = std::make_unique<CGeminiOCR>();
        provider->setTranslate(m_bTranslate);
        m_vLastResults = provider->recognize(m_vLastSelectionPng);
        m_sResultText = "";
        for (const auto& r : m_vLastResults) m_sResultText += r.text + " ";
        markDirty();
    }});
#endif

    defs.push_back({"Copiar", [this]() {
        if (!m_sResultText.empty() && m_sResultText != "Selección lista. Elija una opción.") {
            NClipboard::copy(m_sResultText);
            m_sResultText = "¡Texto copiado!";
            markDirty();
        }
    }});
    
    defs.push_back({"Cerrar", [this]() { finish(); }});

    // Calculate Grid Layout (2 columns)
    int colWidth = (width - (padding * 2) - gap) / 2;
    for (size_t i = 0; i < defs.size(); ++i) {
        int row = i / 2;
        int col = i % 2;
        m_vMenuButtons.push_back({
            defs[i].label, 
            {pos.x + padding + col * (colWidth + gap), pos.y + padding + row * (buttonHeight + gap)}, 
            {(double)colWidth, (double)buttonHeight}, 
            defs[i].action
        });
    }

    int buttonsEnd = padding + ((defs.size() + 1) / 2) * (buttonHeight + gap);
    int totalHeight = buttonsEnd + 150;

    // Background shadow
    cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
    drawRoundedRect(cr, pos.x + 3, pos.y + 3, width, totalHeight, borderRadius);
    cairo_fill(cr);

    // Main Panel
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.12, 0.98);
    drawRoundedRect(cr, pos.x, pos.y, width, totalHeight, borderRadius);
    cairo_fill(cr);
    
    // Border
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.45, 0.6);
    cairo_set_line_width(cr, 1.2);
    drawRoundedRect(cr, pos.x, pos.y, width, totalHeight, borderRadius);
    cairo_stroke(cr);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc;

    // Buttons
    font_desc = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(layout, font_desc);
    pango_font_description_free(font_desc);

    for (size_t i = 0; i < m_vMenuButtons.size(); ++i) {
        const auto& btn = m_vMenuButtons[i];
        if ((int)i == m_iPressedButton) {
            cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 1.0); // Brighter when pressed
        } else {
            cairo_set_source_rgba(cr, 0.22, 0.22, 0.26, 1.0);
        }
        drawRoundedRect(cr, btn.pos.x, btn.pos.y, btn.size.x, btn.size.y, 5);
        cairo_fill(cr);
        
        if ((int)i == m_iPressedButton)
            cairo_set_source_rgba(cr, 0.7, 0.7, 0.9, 0.8);
        else
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.3);
            
        cairo_set_line_width(cr, 0.8);
        drawRoundedRect(cr, btn.pos.x, btn.pos.y, btn.size.x, btn.size.y, 5);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        pango_layout_set_text(layout, btn.label.c_str(), -1);
        int text_w, text_h;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        cairo_move_to(cr, btn.pos.x + (btn.size.x - text_w) / 2, btn.pos.y + (btn.size.y - text_h) / 2);
        pango_cairo_show_layout(cr, layout);
    }

    m_iPressedButton = -1; // Reset for next frame

    // Result Text Area
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 1.0);
    drawRoundedRect(cr, pos.x + padding, pos.y + buttonsEnd + 10, width - padding * 2, 130, 8);
    cairo_fill(cr);
    
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.25);
    drawRoundedRect(cr, pos.x + padding, pos.y + buttonsEnd + 10, width - padding * 2, 130, 8);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.8, 0.8, 0.85);
    pango_layout_set_width(layout, (width - padding * 4) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, m_sResultText.c_str(), -1);
    cairo_move_to(cr, pos.x + padding + 10, pos.y + buttonsEnd + 20);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

bool CCaptura::checkMenuClick(Vector2D clickPos) {
    for (size_t i = 0; i < m_vMenuButtons.size(); ++i) {
        const auto& btn = m_vMenuButtons[i];
        if (clickPos.x >= btn.pos.x && clickPos.x <= btn.pos.x + btn.size.x &&
            clickPos.y >= btn.pos.y && clickPos.y <= btn.pos.y + btn.size.y) {
            m_iPressedButton = i;
            markDirty();
            // We'll reset it after action or in next draw
            btn.action();
            return true;
        }
    }
    m_iPressedButton = -1;
    return false;
}

void CCaptura::initKeyboard() {
    m_pKeyboard->setKeymap([this](CCWlKeyboard* r, wl_keyboard_keymap_format format, int32_t fd, uint32_t size) {
        if (!m_pXKBContext)
            return;

        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            Debug::log(ERR, "Could not recognise keymap format");
            return;
        }

        const char* buf = (const char*)mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            Debug::log(ERR, "Failed to mmap xkb keymap: %d", errno);
            return;
        }

        m_pXKBKeymap = xkb_keymap_new_from_buffer(m_pXKBContext, buf, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

        munmap((void*)buf, size);
        close(fd);

        if (!m_pXKBKeymap) {
            Debug::log(ERR, "Failed to compile xkb keymap");
            return;
        }

        m_pXKBState = xkb_state_new(m_pXKBKeymap);
        if (!m_pXKBState) {
            Debug::log(ERR, "Failed to create xkb state");
            return;
        }
    });

    m_pKeyboard->setKey([this](CCWlKeyboard* r, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        if (m_pXKBState) {
            int32_t XKBKey = xkb_state_key_get_one_sym(m_pXKBState, key + 8);
            if (XKBKey == XKB_KEY_Right)
                m_vLastCoords.x += m_vLastCoords.x < m_pLastSurface->m_pMonitor->size.x;
            else if (XKBKey == XKB_KEY_Left)
                m_vLastCoords.x -= m_vLastCoords.x > 0;
            else if (XKBKey == XKB_KEY_Up)
                m_vLastCoords.y -= m_vLastCoords.y > 0;
            else if (XKBKey == XKB_KEY_Down)
                m_vLastCoords.y += m_vLastCoords.y < m_pLastSurface->m_pMonitor->size.y;
            else if (XKBKey == XKB_KEY_Escape)
                finish(2);
        } else if (key == 1) // Assume keycode 1 is escape
            finish(2);
    });
}

void CCaptura::initMouse() {
    m_pPointer->setEnter([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
        auto x = wl_fixed_to_double(surface_x);
        auto y = wl_fixed_to_double(surface_y);

        m_vLastCoords        = {x, y};
        m_bCoordsInitialized = true;

        for (auto& ls : m_vLayerSurfaces) {
            if (ls->pSurface->resource() == surface) {
                m_pLastSurface = ls.get();
                break;
            }
        }

        if (m_pCursorShapeDevice)
            m_pCursorShapeDevice->sendSetShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);

        markDirty();
    });
    m_pPointer->setLeave([this](CCWlPointer* r, uint32_t timeMs, wl_proxy* surface) {
        for (auto& ls : m_vLayerSurfaces) {
            if (ls->pSurface->resource() == surface) {
                if (m_pLastSurface == ls.get())
                    m_pLastSurface = nullptr;
                break;
            }
        }

        markDirty();
    });
    m_pPointer->setMotion([this](CCWlPointer* r, uint32_t timeMs, wl_fixed_t surface_x, wl_fixed_t surface_y) {
        auto x = wl_fixed_to_double(surface_x);
        auto y = wl_fixed_to_double(surface_y);
        Vector2D pos = {x, y};
        m_vLastCoords = pos;

        if (m_eDragMode != DRAG_NONE) {
            double minX = std::min(m_vSelectionStart.x, m_vSelectionEnd.x);
            double minY = std::min(m_vSelectionStart.y, m_vSelectionEnd.y);
            double maxX = std::max(m_vSelectionStart.x, m_vSelectionEnd.x);
            double maxY = std::max(m_vSelectionStart.y, m_vSelectionEnd.y);

            switch (m_eDragMode) {
                case DRAG_CREATING:
                    m_vSelectionEnd = pos;
                    break;
                case DRAG_MOVING: {
                    Vector2D size = {maxX - minX, maxY - minY};
                    m_vSelectionStart = pos - m_vDragOffset;
                    m_vSelectionEnd = m_vSelectionStart + size;
                    break;
                }
                case DRAG_TOP_LEFT:
                    m_vSelectionStart = pos;
                    break;
                case DRAG_BOTTOM_RIGHT:
                    m_vSelectionEnd = pos;
                    break;
                case DRAG_TOP_RIGHT:
                    m_vSelectionStart.y = pos.y;
                    m_vSelectionEnd.x = pos.x;
                    break;
                case DRAG_BOTTOM_LEFT:
                    m_vSelectionStart.x = pos.x;
                    m_vSelectionEnd.y = pos.y;
                    break;
                default: break;
            }
            markDirty();
        }
    });

    m_pPointer->setButton([this](CCWlPointer* r, uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state) {
        if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (m_bMenuVisible) {
                if (checkMenuClick(m_vLastCoords))
                    return;
                
                // Click outside menu: hide it and continue to selection logic
                m_bMenuVisible = false;
            }

            double minX = std::min(m_vSelectionStart.x, m_vSelectionEnd.x);
            double minY = std::min(m_vSelectionStart.y, m_vSelectionEnd.y);
            double maxX = std::max(m_vSelectionStart.x, m_vSelectionEnd.x);
            double maxY = std::max(m_vSelectionStart.y, m_vSelectionEnd.y);
            
            const double handleSize = 20.0; // Slightly larger for better UX
            Vector2D pos = m_vLastCoords;

            if (pos.distance({minX, minY}) < handleSize) m_eDragMode = DRAG_TOP_LEFT;
            else if (pos.distance({maxX, minY}) < handleSize) m_eDragMode = DRAG_TOP_RIGHT;
            else if (pos.distance({minX, maxY}) < handleSize) m_eDragMode = DRAG_BOTTOM_LEFT;
            else if (pos.distance({maxX, maxY}) < handleSize) m_eDragMode = DRAG_BOTTOM_RIGHT;
            else if (pos.x > minX && pos.x < maxX && pos.y > minY && pos.y < maxY) {
                m_eDragMode = DRAG_MOVING;
                m_vDragOffset = pos - Vector2D(minX, minY);
            } else {
                m_eDragMode = DRAG_CREATING;
                m_vSelectionStart = pos;
                m_vSelectionEnd = pos;
                m_vLastResults.clear();
            }
        } else {
            if (m_eDragMode != DRAG_NONE) {
                m_eDragMode = DRAG_NONE;
                // Normalize selection
                double x1 = std::min(m_vSelectionStart.x, m_vSelectionEnd.x);
                double y1 = std::min(m_vSelectionStart.y, m_vSelectionEnd.y);
                double x2 = std::max(m_vSelectionStart.x, m_vSelectionEnd.x);
                double y2 = std::max(m_vSelectionStart.y, m_vSelectionEnd.y);
                m_vSelectionStart = {x1, y1};
                m_vSelectionEnd = {x2, y2};
                
                finishSelection();
            }
        }
        markDirty();
    });
    m_pPointer->setAxis([this](CCWlPointer* r, uint32_t time, uint32_t axis, wl_fixed_t value) {
        // Zoom removed
    });
}
