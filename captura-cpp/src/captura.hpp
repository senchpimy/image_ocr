#pragma once

#include "defines.hpp"
#include "helpers/LayerSurface.hpp"
#include "helpers/PoolBuffer.hpp"
#include "ocr/OCR.hpp"

class CCaptura {
  public:
    void                                        init();

    std::mutex                                  m_mtTickMutex;

    SP<CCWlCompositor>                          m_pCompositor;
    SP<CCWlRegistry>                            m_pRegistry;
    SP<CCWlShm>                                 m_pSHM;
    SP<CCZwlrLayerShellV1>                      m_pLayerShell;
    SP<CCZwlrScreencopyManagerV1>               m_pScreencopyMgr;
    SP<CCWpCursorShapeManagerV1>                m_pCursorShapeMgr;
    SP<CCWpCursorShapeDeviceV1>                 m_pCursorShapeDevice;
    SP<CCWlSeat>                                m_pSeat;
    SP<CCWlKeyboard>                            m_pKeyboard;
    SP<CCWlPointer>                             m_pPointer;
    SP<CCWpFractionalScaleManagerV1>            m_pFractionalMgr;
    SP<CCWpViewporter>                          m_pViewporter;
    wl_display*                                 m_pWLDisplay = nullptr;

    xkb_context*                                m_pXKBContext = nullptr;
    xkb_keymap*                                 m_pXKBKeymap  = nullptr;
    xkb_state*                                  m_pXKBState   = nullptr;

    bool                                        m_bAutoCopy       = false;
    bool                                        m_bNotify         = false;
    bool                                        m_bRenderInactive = false;
    bool                                        m_bNoFractional   = false;

    bool                                        m_bRunning      = true;
    bool                                        m_bTranslate    = false;
    std::string                                 m_sLang         = "spa";

    std::vector<std::unique_ptr<SMonitor>>      m_vMonitors;
    std::vector<std::unique_ptr<CLayerSurface>> m_vLayerSurfaces;

    CLayerSurface*                              m_pLastSurface;
    std::unique_ptr<IOCRProvider>               m_pOCR;

    Vector2D                                    m_vLastCoords;
    bool                                        m_bCoordsInitialized = false;

    // Selection state
    enum EDragMode {
        DRAG_NONE,
        DRAG_CREATING,
        DRAG_TOP_LEFT,
        DRAG_TOP_RIGHT,
        DRAG_BOTTOM_LEFT,
        DRAG_BOTTOM_RIGHT,
        DRAG_MOVING
    };

    EDragMode                                   m_eDragMode = DRAG_NONE;
    bool                                        m_bIsSelecting = false;
    Vector2D                                    m_vSelectionStart;
    Vector2D                                    m_vSelectionEnd;
    Vector2D                                    m_vLastSelectionMin;
    Vector2D                                    m_vDragOffset;

    std::vector<unsigned char>                  m_vLastSelectionPng;
    std::vector<SOCRResult>                     m_vLastResults;

    // Menu state
    bool                                        m_bMenuVisible = false;
    Vector2D                                    m_vMenuPos;
    std::string                                 m_sResultText = "";
    int                                         m_iPressedButton = -1;
    struct SMenuButton {
        std::string label;
        Vector2D pos;
        Vector2D size;
        std::function<void()> action;
    };
    std::vector<SMenuButton> m_vMenuButtons;

    void                                        renderSurface(CLayerSurface*, bool forceInactive = false);
    void                                        drawMenu(cairo_t*, Vector2D);
    bool                                        checkMenuClick(Vector2D);

    int                                         createPoolFile(size_t, std::string&);
    bool                                        setCloexec(const int&);
    void                                        recheckACK();
    void                                        initKeyboard();
    void                                        initMouse();

    SP<SPoolBuffer>                             getBufferForLS(CLayerSurface*);

    void                                        convertBuffer(SP<SPoolBuffer>);
    void*                                       convert24To32Buffer(SP<SPoolBuffer>);

    void                                        markDirty();

    void                                        finish(int code = 0);
    void                                        finishSelection();

  private:
};

inline std::unique_ptr<CCaptura> g_pCaptura;
