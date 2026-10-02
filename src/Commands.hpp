#pragma once

#include <windows.h>

namespace UltraLight {

// WM_COMMAND identifiers shared by menus, keyboard shortcuts and the toolbar.
enum ControlID : WORD {
    IDC_EDIT_ADDRESS        = 1004,
    IDC_EDIT_FIND           = 1020
};

enum AppCommandID : WORD {
    IDM_FOCUS_ADDRESS_BAR   = 2001,
    IDM_TOGGLE_FULLSCREEN   = 2002,
    IDM_EXIT_FULLSCREEN     = 2003,
    IDM_ZOOM_IN             = 2004,
    IDM_ZOOM_OUT            = 2005,
    IDM_ZOOM_RESET          = 2006,
    IDM_DNS_TOGGLE_ENABLE   = 2010,
    IDM_DNS_OPEN_SETTINGS   = 2011,
    IDM_BLOCKER_PICKER      = 2012,
    IDM_BLOCKER_TOGGLE_NATIVE = 2013,
    IDM_BLOCKER_CLEAR_RULES = 2014,
    IDM_SHARE_COPY_URL      = 2015,
    IDM_SHARE_OPEN_DEFAULT  = 2016,
    IDM_SURROUND_TOGGLE     = 2017,
    IDM_SURROUND_MODE_LIGHT = 2018,
    IDM_SURROUND_MODE_STANDARD = 2019,
    IDM_SURROUND_MODE_CINEMA = 2020,
    IDM_SURROUND_VOCAL_BOOST = 2021,
    IDM_SURROUND_BOOST_100  = 2022,
    IDM_SURROUND_BOOST_150  = 2023,
    IDM_SURROUND_BOOST_200  = 2024,
    IDM_SURROUND_BOOST_300  = 2025,
    IDM_SURROUND_MONO_DOWNMIX = 2026,
    IDM_SURROUND_DEV_AUTO   = 2027,
    IDM_SURROUND_DEV_HEADPHONES = 2028,
    IDM_SURROUND_DEV_SPEAKERS = 2029,
    IDM_ZOOM_SET_BASE       = 2030,  // .. 2046
    IDM_AUDIO_NATIVE        = 2050,
    IDM_AUDIO_ENHANCED      = 2051,
    IDM_AUDIO_WINDOWS_SETTINGS = 2052,
    IDM_AUDIO_DIALOGUE      = 2053,
    IDM_AUDIO_DIAGNOSTICS   = 2054,
    IDM_AUDIO_DEESSER       = 2055,
    IDM_AUDIO_NIGHT         = 2056,
    IDM_UA_DEFAULT          = 2060,
    IDM_UA_MACOS_EDGE       = 2061,
    IDM_UA_SELFTEST         = 2062,
    IDM_ABOUT               = 2063,

    // Browser
    IDM_NEW_TAB             = 2200,
    IDM_NEW_WINDOW          = 2201,
    IDM_NEW_PRIVATE_WINDOW  = 2202,
    IDM_CLOSE_TAB           = 2203,
    IDM_REOPEN_TAB          = 2204,
    IDM_NEXT_TAB            = 2205,
    IDM_PREV_TAB            = 2206,
    IDM_BACK                = 2207,
    IDM_FORWARD             = 2208,
    IDM_RELOAD              = 2209,
    IDM_STOP                = 2210,
    IDM_BOOKMARKS           = 2211,
    IDM_HISTORY             = 2212,
    IDM_DOWNLOADS           = 2213,
    IDM_READING_LIST        = 2214,
    IDM_READER              = 2215,
    IDM_FIND                = 2216,
    IDM_FIND_NEXT           = 2217,
    IDM_FIND_PREV           = 2218,
    IDM_FIND_CLOSE          = 2219,
    IDM_PRIVACY_REPORT      = 2220,
    IDM_SETTINGS            = 2221,
    IDM_SIDEBAR             = 2222,
    IDM_OVERVIEW            = 2223,
    IDM_ADD_BOOKMARK        = 2224,
    IDM_ADD_FAVORITE        = 2225,
    IDM_ADD_READING         = 2226,
    IDM_MENU                = 2227,
    IDM_SHARE               = 2228,
    IDM_CLOSE_WINDOW        = 2229,
    IDM_TAB_DUPLICATE       = 2230,
    IDM_TAB_CLOSE_OTHERS    = 2231,
    IDM_TAB_CLOSE_RIGHT     = 2232,
    IDM_TAB_MUTE            = 2233,
    IDM_TAB_RELOAD          = 2234,
    IDM_TAB_BOOKMARK        = 2235,
    IDM_SUSPEND_OTHERS      = 2236,
    IDM_HOME                = 2237,
    IDM_PRINT               = 2238,

    // Site settings
    IDM_SITE_AUTO_READER    = 2300,
    IDM_SITE_ADBLOCK        = 2301,
    IDM_SITE_CAMERA_ASK     = 2302,
    IDM_SITE_CAMERA_ALLOW   = 2303,
    IDM_SITE_CAMERA_DENY    = 2304,
    IDM_SITE_MIC_ASK        = 2305,
    IDM_SITE_MIC_ALLOW      = 2306,
    IDM_SITE_MIC_DENY       = 2307,
    IDM_SITE_LOC_ASK        = 2308,
    IDM_SITE_LOC_ALLOW      = 2309,
    IDM_SITE_LOC_DENY       = 2310,
    IDM_SITE_POPUP_BLOCK    = 2311,
    IDM_SITE_POPUP_ALLOW    = 2312,
    IDM_SITE_RESET          = 2313,
    IDM_SITE_ZOOM_RESET     = 2314,

    // Power
    IDM_SUSPEND_NEVER       = 2320,
    IDM_SUSPEND_5           = 2321,
    IDM_SUSPEND_10          = 2322,
    IDM_SUSPEND_30          = 2323,
    IDM_SUSPEND_60          = 2324,

    IDM_SELECT_TAB_BASE     = 2400,  // Ctrl+1..9 -> 2401..2409
    IDM_DNS_SELECT_BASE     = 2100   // .. 2149
};

// Maps a key press to a browser command (0 = not a shortcut). Shared by the
// window message loop and WebView2's AcceleratorKeyPressed so shortcuts behave
// the same whether focus is in the toolbar or in the page.
inline WORD MapShortcut(UINT key, bool ctrl, bool shift, bool alt) {
    if (ctrl && !alt) {
        if (!shift) {
            switch (key) {
            case 'T': return IDM_NEW_TAB;
            case 'N': return IDM_NEW_WINDOW;
            case 'W': case VK_F4: return IDM_CLOSE_TAB;
            case 'L': return IDM_FOCUS_ADDRESS_BAR;
            case 'R': return IDM_RELOAD;
            case 'F': return IDM_FIND;
            case 'G': return IDM_FIND_NEXT;
            case 'D': return IDM_ADD_BOOKMARK;
            case 'H': return IDM_HISTORY;
            case 'J': return IDM_DOWNLOADS;
            case VK_OEM_COMMA: return IDM_SETTINGS;
            case VK_TAB: case VK_NEXT: return IDM_NEXT_TAB;
            case VK_PRIOR: return IDM_PREV_TAB;
            case VK_OEM_PLUS: case VK_ADD: return IDM_ZOOM_IN;
            case VK_OEM_MINUS: case VK_SUBTRACT: return IDM_ZOOM_OUT;
            case '0': case VK_NUMPAD0: return IDM_ZOOM_RESET;
            default: break;
            }
            if (key >= '1' && key <= '9') return static_cast<WORD>(IDM_SELECT_TAB_BASE + (key - '0'));
        } else {
            switch (key) {
            case 'N': return IDM_NEW_PRIVATE_WINDOW;
            case 'T': return IDM_REOPEN_TAB;
            case 'W': return IDM_CLOSE_WINDOW;
            case 'R': return IDM_READER;
            case 'G': return IDM_FIND_PREV;
            case 'D': return IDM_ADD_READING;
            case 'B': return IDM_BOOKMARKS;
            case 'L': return IDM_SIDEBAR;
            case 'H': return IDM_BLOCKER_PICKER;
            case VK_OEM_5: return IDM_OVERVIEW;          // Ctrl+Shift+Backslash
            case VK_TAB: return IDM_PREV_TAB;
            case VK_OEM_PLUS: return IDM_ZOOM_IN;
            default: break;
            }
        }
    }
    if (alt && !ctrl && !shift) {
        if (key == VK_LEFT) return IDM_BACK;
        if (key == VK_RIGHT) return IDM_FORWARD;
        if (key == 'D') return IDM_FOCUS_ADDRESS_BAR;
        if (key == VK_HOME) return IDM_HOME;
    }
    if (!ctrl && !alt) {
        if (key == VK_F5) return IDM_RELOAD;
        if (key == VK_F6) return IDM_FOCUS_ADDRESS_BAR;
        if (key == VK_F11) return IDM_TOGGLE_FULLSCREEN;
        if (key == VK_F3) return shift ? IDM_FIND_PREV : IDM_FIND_NEXT;
    }
    return 0;
}

} // namespace UltraLight
