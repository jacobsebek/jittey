#include <Windows.h>
#include <Strsafe.h>
#include <Commctrl.h>
#include <stddef.h>

// We need this for the error_box_format function
#include <stdarg.h>

// The name to be displayed while creating a new file
#define NEW_FILE_NAME L"Empty file"
// A custom window message to signify that the caret of a text-box has moved
#define WM_USER_CARETMOVE (WM_USER+0)
// An eddit control accelerator code to delete the word behind the cursor (Ctrl+Backspace)
#define ACC_EDIT_DELETEWORD 0

// Describes if the string loaded uses '\n' or '\r\n' to signify line breaks
enum linebreak {
    LINEBREAK_UNIX,
    LINEBREAK_WIN
};

// Describes the encoding of a string
enum encoding {
    ENCODING_UTF8,
    ENCODING_UTF16
};

// A so-called format specifies the encoding, linebreak type and the BOM
struct format {
    enum encoding encoding;
    enum linebreak linebreak;
    BOOL bom;
};

// The format used by the internal text-box
CONST struct format Internal_format = {
    .encoding = ENCODING_UTF16,
    .linebreak = LINEBREAK_WIN,
    .bom = FALSE
};

struct format Default_format = {
    .encoding = ENCODING_UTF8,
    .bom = FALSE,
    .linebreak = LINEBREAK_WIN
};

// The byte-order-mark structure, size is in bytes
struct bom { UINT32 data; SIZE_T size; };

// The main and only window
static HWND Window = NULL;
// The size, in pixels, of the window's client area
static int Width = 640, Height = 480;

// The preferred fonts to use for the window elements
// The add_* functions use these fonts for new controls
static struct {
    HFONT editor, filename;
} Fonts;

// These values are used as ID's to the GUI elements
enum Gui_Enums {
    GUI_TEXT_BOX, GUI_STATIC_TEXT,
    GUI_MENU_NEW, GUI_MENU_LOAD, GUI_MENU_SAVE, GUI_MENU_ABOUT, GUI_MENU_WWRAP
};

// A singleton structure that holds all needed handles to the GUI elements 
static struct {
    HWND text_box, filename, status;
    HMENU menu, menu_file, menu_edit, menu_help;
    HACCEL edit_accels;
} Gui;

// Hold information about layout and spacing of the GUI
static struct {
    INT filename_height, margin, reduced_margin;
} Layout;

// Hold information about the settings of the current file
// This is used when saving the file, to save it in the original format
static struct {
    struct format format;
    BOOL is_new;
} Settings;

// Show a formatted MessageBox with the latest error obtained by GetLastError()
static void error_box_winerror(PCWSTR caption) {

    DWORD err = GetLastError();

    WCHAR buf[128];
    if (FAILED(StringCbPrintfW(buf, sizeof(buf), L"%ls\n%d (0x%X)", caption, err, err)))
        if (FAILED(StringCbCopyW(buf, sizeof(buf), L"Failed to format the error message")))
            buf[0] = L'\0';
    
    MessageBoxW(Window, 
                buf,
                L"Unexpected error",
                MB_OK | MB_ICONERROR | MB_DEFBUTTON1 | MB_APPLMODAL);

}

// Same as error_box_winerror but terminates the process, returning the last error code
static void fatal(PCWSTR caption) {
    error_box_winerror(caption);
    ExitProcess(GetLastError());
}

// A MessageBox wrapper, displays an error box
static void error_box(PCWSTR caption, PCWSTR msg) {
    MessageBoxW(Window, 
               msg,
               caption,
               MB_OK | MB_ICONERROR | MB_DEFBUTTON1 | MB_APPLMODAL);
}

// Same as error_box, formats the 'msg' argument (like printf)
static void error_box_format(PCWSTR caption, PCWSTR msg, ...) {

    va_list args;
    va_start(args, msg);

    WCHAR buf[256];
    if (FAILED(StringCbVPrintfW(buf, sizeof(buf), msg, args)))
        if (FAILED(StringCbCopyW(buf, sizeof(buf), L"Failed to format the error message")))
            buf[0] = L'\0';

    MessageBoxW(Window, 
               buf,
               caption,
               MB_OK | MB_ICONERROR | MB_DEFBUTTON1 | MB_APPLMODAL);

    va_end(args);    
}

// Updates the status bar's proportions according to the width of the window
static void resize_status_bar() {
    INT sizes[4] = {Width-330, Width-230, Width-130, -1};
    SendMessageW(Gui.status, SB_SETPARTS, (WPARAM)4, (LPARAM)sizes);
    SendMessageW(Gui.status, WM_SIZE, 0, 0);
}

// Adds an already set-up status bar to the window
static HWND add_status_bar() {
    HWND sbar = CreateWindowW(
            STATUSCLASSNAMEW,
            L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            Window,
            NULL,
            (HINSTANCE)GetWindowLongPtr(Window, GWLP_HINSTANCE),
            NULL
        );
    if (!sbar)
        fatal(L"Failed to create the status bar");

    return sbar;
}

// Change the format displayed on the status bar (and thus even the global current file settings)
static void change_format(struct format format) {

    // Change the type variable itself
    Settings.format = format;

    static PCWSTR encodings[] = {
        [ENCODING_UTF8 ] = L"UTF-8",
        [ENCODING_UTF16] = L"UTF-16"
    };

    WCHAR buf[128];
    // Format the encoding type
    StringCbPrintfW(buf, sizeof(buf), L"%ls%ls", encodings[Settings.format.encoding], Settings.format.bom ? L" with BOM" : L"");
    SendMessageW(Gui.status, SB_SETTEXTW, 3, (LPARAM)buf);

    // Format the linebreak type
    StringCbPrintfW(buf, sizeof(buf), L"%ls", Settings.format.linebreak == LINEBREAK_UNIX ? L"Unix (LF)" : L"Windows (CRLF)");
    SendMessageW(Gui.status, SB_SETTEXTW, 2, (LPARAM)buf);
}

// Change the cursor position displayed on the status bar
static void change_status_pos(ULONGLONG row, ULONGLONG col) {
    WCHAR buf[128];

    // Format the lines and columns
    StringCbPrintfW(buf, sizeof(buf), L"Ln %llu, Col %llu", row, col);
    SendMessageW(Gui.status, SB_SETTEXTW, 1, (LPARAM)buf);
}

// A custom edit control procedure used by all edit controls created by the add_text_box function,
// it supports the ACC_EDIT_DELETEWORD accelerator and sends WM_USER_CARETMOVE to the main window
static LRESULT CALLBACK EditProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

    switch (uMsg) {
        // If the user presses a key or clicks the mouse, the caret position has likely changed
        //TODO: react to changes while dragging the mouse in real time (WM_MOUSEMOVE would be possible
        // but is unnecessarily frequent)
        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:  
        case EM_SETSEL:
        case WM_CLEAR:
            // Checking the return value is possible and recommended, but unnecessary
            // If the message queue is full, there are other things to worry about than the caret moving
            // PostMessage is necessary here (don't ask me why) // TODO: find out why PostMessage is necessary here
            PostMessageW(Window, WM_USER_CARETMOVE, MAKEWPARAM(GetDlgCtrlID(hwnd), 0), (LPARAM)hwnd);
        break;
        case WM_COMMAND:
            switch(HIWORD(wParam)) {
                case 1:
                    switch (LOWORD(wParam)) {
                        // TODO: fails when trying to delete the first word with a BOM present, 
                        // load files without the bom
                        case ACC_EDIT_DELETEWORD: {                                
                            HLOCAL textH = (HLOCAL)SendMessageW(hwnd, EM_GETHANDLE, 0, 0);
                            PCWSTR text = LocalLock(textH);
                            CONST ULONGLONG text_length = GetWindowTextLengthW(hwnd);

                            // Find the last whitespace character
                            DWORD sel_start, sel_end;
                            SendMessageW(hwnd, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
                            if (sel_start != sel_end) {
                                SendMessageW(hwnd, WM_CLEAR, 0, 0);
                                break;
                            }

                            ULONGLONG start = sel_start;
                            for (; start > 0 && iswspace(text[start-1]); start--);
                            for (; start > 0 && !iswspace(text[start-1]); start--);

                            // Delete the last word
                            // TODO: the edit control flickers when redrawn
                            SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
                                SendMessageW(hwnd, EM_SETSEL, start, sel_start);
                                SendMessageW(hwnd, WM_CLEAR, 0, 0);
                            SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);

                            LocalUnlock(textH);

                            return 0;
                        } break;
                    }
                break;
            }
        break;
    }

    return CallWindowProcW((WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA), hwnd, uMsg, wParam, lParam);
}

// Adds an edit control to the main window (unscaled, unpositioned, check the resize() method)
static HWND add_text_box(CONST UINT id, BOOL wrap) {
    HWND text_box = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL | (wrap ? ES_AUTOHSCROLL | WS_HSCROLL : 0),
        0,0,0,0,
        Window,
        (HMENU)(DWORD_PTR)id,
        (HINSTANCE)GetWindowLongPtr(Window, GWLP_HINSTANCE),
        NULL
    );
    if (!text_box)
        fatal(L"Failed to create the text box");

    SendMessageW(text_box, WM_SETFONT, (WPARAM)Fonts.editor, TRUE);
    SetWindowLongPtrW(text_box, GWLP_USERDATA, (LONG_PTR)SetWindowLongPtrW(text_box, GWLP_WNDPROC, (LONG_PTR)EditProc));

    return text_box;
}

// Adds a static text to the main window (unscaled, unpositioned, check the resize() method)
static HWND add_static_text(CONST INT id) {

    HWND static_text = CreateWindowW(
        WC_STATICW,
        L"",
        WS_CHILD | WS_VISIBLE | SS_SIMPLE,
        0, 0, 0, 0,
        Window,
        (HMENU)(DWORD_PTR)id,
        (HINSTANCE)GetWindowLongPtr(Window, GWLP_HINSTANCE),
        NULL
    );
    if (!static_text)
        fatal(L"Failed to create the static text");

    SendMessageW(static_text, WM_SETFONT, (WPARAM)Fonts.filename, TRUE);

    return static_text;

}

// Adds a button with a title with an ID to a menu
static void add_menu_button(HMENU menu, UINT id, PCWSTR title) {
    MENUITEMINFOW info;
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STRING | MIIM_ID;
    info.wID = id;
    info.dwTypeData = (PWSTR)title;

    if (!(InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info)))
        fatal(L"Failed to insert the menu button");
}

// Inserts a button with a title, ID and a checkbox to the menu (the default state is unchecked)
static void add_menu_checkbox(HMENU menu, UINT id, PCWSTR title) {
    MENUITEMINFOW info;
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STRING | MIIM_ID | MIIM_CHECKMARKS | MIIM_STATE;
    // The checkmark info
    info.hbmpChecked = NULL;
    info.hbmpUnchecked = NULL;
    info.fState = MFS_UNCHECKED;
    // The common info
    info.wID = id;
    info.dwTypeData = (PWSTR)title;

    if (!InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info))
        fatal(L"Failed to insert a menu checkbox");
}

// Adds a submenu to a menu item
static void add_menu_submenu(HMENU menu, HMENU submenu, PCWSTR title) {
    MENUITEMINFOW info;
    info.cbSize = sizeof(MENUITEMINFOW);
    info.fMask = MIIM_STRING | MIIM_SUBMENU;
    info.hSubMenu = submenu;
    info.dwTypeData = (PWSTR)title;

    if (!(InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info)))
        fatal(L"Failed to insert a menu submenu");
}

// Changes the string shown in the static text above the text-box
static void change_filename(PCWSTR fname) {
    // Set the static text to the file name
    SetWindowTextW(Gui.filename, fname);

    // Invaliate the file name area (it has to be redrawn because it's transparent)
    RECT wr;
    GetClientRect(Gui.filename, &wr);
    MapWindowPoints(Gui.filename, Window, (PPOINT)&wr, 2);
    InvalidateRect(Window, &wr, TRUE);
}

// Resizes and repositions all controls based on the current proportions (The Layout structure)
static void resize() {
    static BOOL b = 0;
    // Retrieve the size of the status bar (only the height matters)
    RECT status_rect;
    SendMessageW(Gui.status, SB_GETRECT, 0, (LPARAM)&status_rect);

    // Resize the file name static control accordingly
    SetWindowPos(
        Gui.filename, NULL, 
        Layout.margin, 
        Layout.reduced_margin, 
        Width-Layout.margin*2, 
        Layout.filename_height,
        SWP_NOZORDER);
    // Resize the text box itself accordingly
    SetWindowPos(Gui.text_box , NULL, 
        Layout.margin, 
        Layout.reduced_margin*2+Layout.filename_height, 
        Width-Layout.margin*2, 
        Height-Layout.reduced_margin*3-Layout.filename_height-(status_rect.bottom-status_rect.top), 
        SWP_NOZORDER);

    // Resize the status bar
    resize_status_bar(Gui.status);
}

// Toggles word wrapping, this function actually has to create a new text-box and copy the data
static void toggle_wwrap() {
    // Toggle it in the menu

    BOOL wrap;
    {
        MENUITEMINFOW info;
        info.cbSize = sizeof(info);

        info.fMask = MIIM_STATE;
        GetMenuItemInfoW(Gui.menu_edit, GUI_MENU_WWRAP, FALSE, &info);
        wrap = info.fState & MFS_CHECKED;

        info.fState = (wrap ? MFS_UNCHECKED : MFS_CHECKED);
        if (!SetMenuItemInfoW(Gui.menu_edit, GUI_MENU_WWRAP, FALSE, &info))
            fatal(L"Toggle change word-wrap");
    }

    // Toggle the actual style
    // I cannot dynamically change the word wrap so I have to create a new text box

    // Create a new box
    HWND newtbox = add_text_box(GUI_TEXT_BOX, wrap);

    // Copy the window text (I am intentionally not using an intermediate buffer)
    {
        HLOCAL textH = (HLOCAL)SendMessage(Gui.text_box, EM_GETHANDLE, 0, 0);
        PCWSTR text = LocalLock(textH);
        SetWindowTextW(newtbox, text);
        LocalUnlock(textH);
    }

    // Destroy the old box and assign the new one
    DestroyWindow(Gui.text_box);
    Gui.text_box = newtbox;
    
    // Reposition the gui, including our new box
    resize();
}

// Prompts the user with an GetOpenFileName or a GetSaveFileName based on the argument (the former if it's 0)
// The returned pointer is an internal static buffer, so there is no need to free it but it will change after the next chooose_file call
static PCWSTR choose_file(CONST BOOL save) {

    static WCHAR buf[512];

    static OPENFILENAMEW opts;

    opts.lStructSize = sizeof(OPENFILENAMEW);
    opts.hwndOwner = Window;
    opts.hInstance = (HINSTANCE)GetWindowLongPtr(Window, GWLP_HINSTANCE);
    //opts.lpstrFilter = NULL;
    opts.lpstrFilter = L"Text documents (*.txt)\0*.txt\0All files (*)\0*\0";
    opts.lpstrCustomFilter = NULL;
    opts.nFilterIndex = 2; // start on "all files" filter

    opts.lpstrFile = buf;
    // Set the default save location as the path of the current file, if the file is new, don't
    if (Settings.is_new)
        opts.lpstrFile[0] = L'\0';
    else
        GetWindowTextW(Gui.filename, opts.lpstrFile, sizeof(buf));

    opts.nMaxFile = sizeof(buf);
    opts.lpstrFileTitle = NULL;
    opts.lpstrInitialDir = NULL;
    opts.lpstrTitle = NULL;
    //opts.Flags = OFN_OVERWRITEPROMPT;
    opts.nFileOffset = 0;
    opts.nFileExtension = 5;
    //opts.lpstrDefExt = L"txt";
    opts.lpstrDefExt = NULL;
    opts.FlagsEx = 0;

    if (save) {
        if (!GetSaveFileNameW(&opts)) return NULL;
    } else {
        if (!GetOpenFileNameW(&opts)) return NULL;
    }

    return opts.lpstrFile;
}

// Get's a standard BOM for the specified encoding
static struct bom get_bom(enum encoding encoding) {
    struct bom bom;

    switch (encoding) {
        case ENCODING_UTF16:
            bom.data = 0xFEFF;
            bom.size = 2;
        break;
        case ENCODING_UTF8:
            bom.data = 0xBFBBEF;
            bom.size = 3;
        break;
        default:
            bom.data = 0;
            bom.size = 0;
        break;
    }

    return bom;
}

// Converts a string from a specified format to a specified format
// If the 'nullterm' argument is FALSE, the returned string is not guaranteed to be null-terminated and the new_size variable is set to the size without the null terminator
// If the 'src_should_free' flag is TRUE, the 'src' argument is guaranteed to be freed using HeapFree after the conversion
// The new_size pointer points to a valid memory address or NULL, if it is not NULL, it is set to the size of the returned buffer
static PVOID convert(PVOID src, struct format from, struct format to, BOOL nullterm, BOOL src_should_free, SIZE_T* new_size) {

    // The conversion (intermediate) buffer
    SIZE_T inter_size = 0;
    PWSTR inter = NULL;
    BOOL inter_should_free = FALSE; // must be initialized because of the 'quit' label
    BOOL fail = FALSE;

    CONST struct bom from_bom = from.bom ? get_bom(from.encoding) : (struct bom){0};
    CONST struct bom to_bom   = to.bom   ? get_bom(to.encoding)   : (struct bom){0};

    // Convert the source to UTF-16 (skip the BOM)
    switch (from.encoding) {
        case ENCODING_UTF16:
            // We don't have to do anything if the source itself is in the right encoding
            inter = (PWSTR)((PBYTE)src + from_bom.size);
            inter_size = (lstrlenW((PCWSTR)src) + 1) * sizeof(WCHAR) - from_bom.size;
            inter_should_free = FALSE;
        break;
        case ENCODING_UTF8: {
            
            // We have to handle this because the used winapi doesn't like empty strings...
            if (((PCHAR)src)[0] == '\0') {

                inter_size = sizeof(WCHAR);
                if (!(inter = HeapAlloc(GetProcessHeap(), 0, inter_size)))
                    fatal(L"Failed to allocate the conversion buffer");
                inter_should_free = TRUE;

                ((PWCHAR)inter)[0] = L'\0';
                break;
            }

            // Firstly, let's do a dry run to determine the size of the output
            SIZE_T inter_length;
            if (!(inter_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (PBYTE)src+from_bom.size, -1, NULL, 0))) {
                error_box_winerror(L"Invalid encoding");
                fail = TRUE;
                goto quit;
            }

            // Allocate the destination buffer
            if (!(inter = HeapAlloc(GetProcessHeap(), 0, inter_length * sizeof(WCHAR))))
                fatal(L"Failed to allocate the conversion buffer");
            inter_should_free = TRUE;

            // The actual conversion
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (PBYTE)src+from_bom.size, -1, inter, inter_length) != inter_length) {
                error_box_winerror(L"Invalid encoding");
                fail = TRUE;
                goto quit;
            }

            inter_size = inter_length * sizeof(WCHAR);
        } break;
    }

    // Convert the linebreaks
    // Note that we don't care about the input linebreak format at all
    switch (to.linebreak) {
        case LINEBREAK_WIN: {

            SIZE_T newinter_length = 0;
            BOOL skip = TRUE;
            for (CONST WCHAR* wc = inter; *wc; wc++) {
                if (*wc == L'\n' && (wc-inter == 0 || *(wc-1) != L'\r')) {
                    skip = FALSE;
                    newinter_length++;
                }

                newinter_length++;
            }

            if (skip) break; // If all of the newlines are CRLF, we can skip this and save a reallocation

            PWSTR newinter;
            if (!(newinter = HeapAlloc(GetProcessHeap(), 0, (newinter_length + 1) * sizeof(WCHAR))))
                fatal(L"Failed to allocate the conversion buffer");

            // Write to the new buffer with corrected newlines
            {
                PWCHAR dc = newinter;
                PCWCHAR sc = inter;
                for (; *sc; dc++, sc++) {
                    if (*sc == L'\n' && (sc-inter == 0 || *(sc-1) != L'\r'))
                        *(dc++) = L'\r';

                    *dc = *sc; 
                }
                *dc = L'\0';
            }

            if (inter_should_free && !HeapFree(GetProcessHeap(), 0, inter))
                fatal(L"Failed to free the conversion buffer");

            inter = newinter;
            inter_size = (newinter_length + 1) * sizeof(WCHAR);
            inter_should_free = TRUE;
        } break;
        case LINEBREAK_UNIX: {
            // First, count the amount of characters needed
            SIZE_T newinter_length = 0;
            BOOL skip = TRUE;
            for (PCWCHAR wc = inter; *wc; wc++) {
                if (!wcsncmp(wc, L"\r\n", 2)) { // out of bounds checks unnecessary (null at the end)
                    skip = FALSE;
                    newinter_length--;
                }

                newinter_length++;
            }

            if (skip) break; // we can skip all of this if there are no windows type linebreaks

            PWSTR newinter;
            if (!(newinter = HeapAlloc(GetProcessHeap(), 0, (newinter_length + 1) * sizeof(WCHAR))))
                fatal(L"Failed to allocate the conversion buffer");

            // Write to the new buffer with corrected newlines
            {
                PWCHAR dc = newinter;
                PCWCHAR sc = inter;
                for (; *sc; dc++, sc++) {
                    if (!wcsncmp(sc, L"\r\n", 2)) {
                        dc--;
                        continue;
                    }

                    *dc = *sc; 
                }
                *dc = L'\0';
            }

            if (inter_should_free && !HeapFree(GetProcessHeap(), 0, inter))
                fatal(L"Failed to free the conversion buffer");

            inter = newinter;
            inter_size = (newinter_length + 1) * sizeof(WCHAR);
            inter_should_free = TRUE;
        } break;
    }

    // Convert to the target encoding
    switch (to.encoding) {
        // Because the intermediate string is UTF-16, we pretty much just copy it
        case ENCODING_UTF16: {
            
            SIZE_T newinter_size = inter_size + to_bom.size - (!nullterm)*sizeof(WCHAR);
            PVOID newinter;
            if (!(newinter = HeapAlloc(GetProcessHeap(), 0, newinter_size)))
                fatal(L"Failed to allocate the conversion buffer");

            // Add the BOM
            memcpy(newinter, &to_bom.data, to_bom.size);
            // Add the string itself
            memcpy((PBYTE)newinter + to_bom.size, inter, inter_size - (!nullterm)*sizeof(WCHAR));

            // Free the intermediate buffer
            if (inter_should_free && !HeapFree(GetProcessHeap(), 0, inter))
                fatal(L"Failed to free the conversion buffer");

            inter = newinter;
            inter_size = newinter_size;
            inter_should_free = FALSE; // this is the buffer getting returned
        } break;
        case ENCODING_UTF8: {

            SIZE_T newinter_size;
            PVOID newinter;

            // We have to handle this because the used winapi doesn't like empty strings...
            if (inter[0] == L'\0') {

                newinter_size = nullterm ? sizeof(CHAR) : 0;
                if (!(newinter = HeapAlloc(GetProcessHeap(), 0, newinter_size)))
                    fatal(L"Failed to allocate the conversion buffer");

                if (nullterm) ((PCHAR)newinter)[0] = '\0';
            } else {

                // Firstly, let's do a dry run to determine the size of the output
                if (!(newinter_size = WideCharToMultiByte(CP_UTF8, 0, inter, inter_size/sizeof(WCHAR)-(!nullterm), NULL, 0, NULL, NULL))) {
                    error_box_winerror(L"Invalid encoding");
                    fail = TRUE;
                    goto quit;
                }
                newinter_size += to_bom.size; // count in the bom size

                // Allocate the destination buffer
                if (!(newinter = HeapAlloc(GetProcessHeap(), 0, newinter_size)))
                    fatal(L"Failed to allocate the conversion buffer");

                // Add the BOM
                memcpy(newinter, &to_bom.data, to_bom.size);
                // The actual conversion
                // TODO: Look at the length of this, god please
                if (WideCharToMultiByte(CP_UTF8, 0, inter, inter_size/sizeof(WCHAR)-(!nullterm), (PBYTE)newinter+to_bom.size, newinter_size-to_bom.size, NULL, NULL) != newinter_size-to_bom.size) {
                    error_box_winerror(L"Failed to convert the input string");
                    fail = TRUE;
                    goto quit;
                }
            }

            // Free the intermediate buffer
            if (inter_should_free && !HeapFree(GetProcessHeap(), 0, inter))
                fatal(L"Failed to free the conversion buffer");

            inter = newinter;
            inter_size = newinter_size;
            inter_should_free = FALSE; // this is the buffer getting returned
        } break;
    }

    // Handy label for when we quit unexpectedly
    // Don't forget to set the 'fail' flag if we do though, it will make sure that the buffers get freed AND that we return NULL + 0
    quit:

    // Free the intermediate buffer
    if (inter_should_free) {
        if (!HeapFree(GetProcessHeap(), 0, inter))
            fatal(L"Failed to free the conversion buffer");
        inter = NULL;
    }

    // Free the source buffer if the user desires
    if (src_should_free && !HeapFree(GetProcessHeap(), 0, src))
        fatal(L"Failed to free the conversion buffer");

    if (new_size)
        *new_size = fail ? 0 : inter_size;

    return fail ? NULL : inter;
}

// Guesses the format of the input string
static struct format get_format(PCSTR src, CONST SIZE_T src_size) {

    struct format format = {0};

    if (IsTextUnicode(src, src_size, NULL)) {
        format.encoding = ENCODING_UTF16;
        struct bom bom = get_bom(ENCODING_UTF16); // get the BOM for utf-16 (LE)

        // If the source bigger or equally big as the BOM, we can safely check it's presence
        if (src_size >= bom.size)
            format.bom = !memcmp(src, &bom.data, bom.size);
        
        if (!format.bom) // necessary for the next operation
            bom.size = 0;

        format.linebreak = LINEBREAK_WIN;
        CONST WCHAR* start = (CONST WCHAR*)(src + bom.size);
        CONST WCHAR* wc = start;
        while (wc = wcschr(wc+1, L'\n')) {
            if (wc - start == 0 || *(wc-1) != L'\r') {
                format.linebreak = LINEBREAK_UNIX;
                break;
            }
        }

    } else {
        // For comments about this section, just look above, it's the same thing, just for UTF-8

        format.encoding = ENCODING_UTF8;
        struct bom bom = get_bom(ENCODING_UTF8);

         if (src_size >= bom.size)
            format.bom = !memcmp(src, &bom.data, bom.size);

        format.linebreak = LINEBREAK_WIN;
        CONST CHAR* start = (CONST CHAR*)(src + bom.size);
        CONST CHAR* c = start;
        while (c = strchr(c+1, '\n')) {
            if (c - start == 0 || *(c-1) != '\r') {
                format.linebreak = LINEBREAK_UNIX;
                break;
            }
        }

    }

    return format;
}

//TODO: you cannot change the encoding a file is saved/opened in, you can only save files in the default format
// unless you have loaded it in a different one, this would require customising the choose_file dialog
// Saves the contents of Gui.text_box to a file with the specified file, overwriting or creating a new file
// Returns TRUE if succeeds
static void save_to_file(PCWSTR fpath) {
    if (!fpath) return;

    // Note that I am purposefully not retrieving the handle itself using EM_GETHANDLE because
    // it would introduce unnecessary complications regarding freeing of this handle, even though
    // it would save one reallocation...
    SIZE_T src_size = (GetWindowTextLengthW(Gui.text_box)+1)*sizeof(WCHAR);
    PVOID src;
    if (!(src = HeapAlloc(GetProcessHeap(), 0, src_size)))
        fatal(L"Failed to allocate the conversion buffer");

    // Get the text
    GetWindowTextW(Gui.text_box, src, src_size/sizeof(WCHAR));

    // Convert the text into the target format

    // TODO: note that because of the linebreak stuff going on, if you load a file
    // that contains a combination of UNIX and windows type linebreaks, when saving
    // the whole file is going to be safed with UNIX linebreaks. The only time when it's
    // going to be saved with windows linebreaks is when there is not a single LF in the file.
    // This is obviously horrendous, because it rewrites parts of the file that the user hasn't even touched.
    // To fix this, A LOT of work would have to be done. Plus this problem is in many cases not solvable.
    // Write the optional BOM and the actual text buffer
    src = convert(src, Internal_format, Settings.format, FALSE, TRUE, &src_size);
    if (!src) return;

    // Open the save file
    HANDLE out = CreateFileW(fpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        error_box_winerror(L"Failed to open the output file");
        return;
    }

    DWORD numwritten;
    if (!WriteFile(out, src, src_size, &numwritten, NULL) || numwritten != src_size)
        fatal(L"Failed to write into the output file");

    if (!CloseHandle(out)) 
        fatal(L"Failed to close file handle");
    
    change_filename(fpath);
    Settings.is_new = FALSE;
}

static void new_file() {
    SetWindowTextW(Gui.text_box, L"");
    change_filename(NEW_FILE_NAME);
    change_format(Default_format);
    change_status_pos(1, 1);
    Settings.is_new = TRUE;
}

// Loads the contents of a file to the Gui.text_box, including the necessary conversions and manipulation, overwriting it
static void load_from_file(PCWSTR fpath) {
    if (!fpath) return;

    // Open the specified file (despite the function name)
    HANDLE in = CreateFileW(   fpath,
                               GENERIC_READ,
                               0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
                               NULL);
    if (in == INVALID_HANDLE_VALUE) {
        error_box_winerror(L"Failed to open the input file");
        return;
    }

    // Get the file size
    LARGE_INTEGER filesize;
    if (!GetFileSizeEx(in, &filesize))
        fatal(L"Failed to retrieve file size");

    CONST SIZE_T src_size = filesize.QuadPart;

    // Check if it's not empty (because MultiByeToWideChar fails with empty buffers)
    if (src_size == 0) {
        SetWindowTextW(Gui.text_box, L"");
        goto quit;
    }

    // Check if it's not too big
    CONST SIZE_T maxchars = SendMessageW(Gui.text_box, EM_GETLIMITTEXT, 0, 0);
    if (src_size > maxchars * sizeof(WCHAR)) {
        error_box_format(
            L"Failed to open the specified file", 
            L"The file is too big (%d bytes!) Max file size is %d bytes (%d characters)", 
            src_size, 
            maxchars * sizeof(WCHAR), 
            maxchars);
        
        goto quit;
    }

    // Allocate the read buffer
    PVOID src;
    if (!(src = HeapAlloc(GetProcessHeap(), 0, src_size+sizeof(WCHAR) ))) // + *possible* wide null terminator
        fatal(L"Failed to allocate the read buffer");

    // Read from the file
    DWORD numread;
    if (!ReadFile(in, src, src_size, &numread, NULL) || numread != src_size)
        fatal(L"Failed to read the input file");

    // Add the null terminator
    memset((PBYTE)src+src_size, 0, sizeof(WCHAR));

    // Deal with file format
    struct format source_format = get_format(src, src_size);
    change_format(source_format);

    PWSTR converted = convert(src, source_format, Internal_format, TRUE, TRUE, NULL);
    if (!converted)
        goto quit;

    SetWindowTextW(Gui.text_box, converted);

    if (!HeapFree(GetProcessHeap(), 0, converted))
        fatal(L"Failed to free the conversion buffer");

    quit:

    if (!CloseHandle(in)) 
        fatal(L"Failed to close file handle");

    change_filename(fpath);
    Settings.is_new = FALSE;
}

// The procedure used for the main window, can be used for only one window because it uses the global variable 'Window' internally
static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

    switch (uMsg) {
        case WM_CREATE:
            Window = hwnd;

            // Set up the layout constants
            //TODO: yes, this is hardcoded and doesn't respond to DPI changes
            Layout.margin = 10;
            Layout.reduced_margin = 5;

            // Setup fonts
            // Get the theme-specific default system fonts
            {
                NONCLIENTMETRICSW nonclient_metrics;
                nonclient_metrics.cbSize = sizeof(NONCLIENTMETRICSW);
                if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &nonclient_metrics, 0))
                    fatal(L"Failed to retrieve the Non-client metrics");

                //TODO: yes, this is hardcoded
                Layout.filename_height = 15;
                Fonts.filename = CreateFontIndirectW(&nonclient_metrics.lfStatusFont);

                // Scale the Font editor text just a tad
                //nonclient_metrics.lfSmCaptionFont.lfHeight *= 1.1;
                //Fonts.editor = CreateFontIndirectW(&nonclient_metrics.lfSmCaptionFont);
                //Fonts.editor = GetStockObject(SYSTEM_FIXED_FONT);

                // Attempt to set Consolas 14 as the font
                LOGFONTW font = {0};
                StringCbCopyW(font.lfFaceName, LF_FACESIZE, L"Consolas");
                font.lfHeight = 14;
                Fonts.editor = CreateFontIndirectW(&font);
            }

            // Add the static control
            Gui.filename = add_static_text(GUI_STATIC_TEXT);

            // Add the text_box
            Gui.text_box = add_text_box(GUI_TEXT_BOX, TRUE);
            SetFocus(Gui.text_box);

            // Create the menu bar
            Gui.menu = CreateMenu();

            // Create the "File" submenu
            Gui.menu_file = CreateMenu();
            // Populate the "File" submenu
            add_menu_button(Gui.menu_file, GUI_MENU_NEW, L"New");
            add_menu_button(Gui.menu_file, GUI_MENU_LOAD, L"Open");
            add_menu_button(Gui.menu_file, GUI_MENU_SAVE, L"Save");

            // Create the "Edit" submenu
            Gui.menu_edit = CreateMenu();
            // Add the "word-wrap" checkbox
            add_menu_checkbox(Gui.menu_edit, GUI_MENU_WWRAP, L"Word Wrap");
            toggle_wwrap(); //TODO: yes, I am creating the text box two times in the beginning

            // Create the "Help" submenu
            Gui.menu_help = CreateMenu();
            add_menu_button(Gui.menu_help, GUI_MENU_ABOUT, L"About");

            // Construct the main menu bar
            add_menu_submenu(Gui.menu, Gui.menu_file, L"File");
            add_menu_submenu(Gui.menu, Gui.menu_edit, L"Edit");
            add_menu_submenu(Gui.menu, Gui.menu_help, L"Help");

            // Slap the menu onto our window
            SetMenu(Window, Gui.menu);

            // Create the status bar
            Gui.status = add_status_bar();
            // we have to do this now because the parts of the bar aren't even there yet
            resize_status_bar();

            // Emulate a window resize to initialise element positions
            resize();
            // Open a new, empty file
            new_file();
            // Finally, show the constructed window and repaint it
            ShowWindow(Window, TRUE);
            UpdateWindow(Window);

        break;
        case WM_DESTROY:
            PostQuitMessage(0);
        break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
        break;
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;

            Width = LOWORD(lParam);
            Height = HIWORD(lParam);

            resize();

        } break;
        case WM_GETMINMAXINFO: {

            PMINMAXINFO mmi = (PMINMAXINFO) lParam;

            // Set the minimum window size
            mmi->ptMinTrackSize.x = 320;
            mmi->ptMinTrackSize.y = 240;

        } break; 
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            // Draw transparent background for the file name control
            // (And all other static controls)

            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(dc, TRANSPARENT);

            return (LRESULT)GetStockObject(NULL_BRUSH);
        } break;
        // A custom message that is generated by our subclassed edit-control
        case WM_USER_CARETMOVE : {
            //TODO: still clunky with selections, doesn't know the position of the cursor itself, only the selection
            // therefore, the status position shown in fact shows only the start of the selection and not the actual caret position
            //TODO: note that this provides the position IN THE TEXTBOX, i.e. if Word Wrap is enabled,
            //it doesn't provide the logical position, this can be achieved by manually counting the newlines
            // Calculate current row
            ULONGLONG row = SendMessageW(Gui.text_box, EM_LINEFROMCHAR, -1, 0);

            // Calculate the current column
            // There is supposedly no way to get the actual caret position, only the selection
            // This could be solved by maybe tracking how the selection changes but still, it's clunky
            DWORD start;
            SendMessageW(Gui.text_box, EM_GETSEL, (WPARAM)&start, (LPARAM)NULL);
            // We have to do this loop beacause EM_LINEINDEX IS aware of the real caret position while getsel isn't
            // If col < 0, we know that the actual caret is on one of the preceding lines
            LONGLONG col;
            do {
                col = (LONGLONG)start - SendMessageW(Gui.text_box, EM_LINEINDEX, row, 0);
            // the point is that row doesn't decrement on the last iteration, it has nothing to do with the logical condition
            } while (col < 0 && row--);

            change_status_pos(row+1, col+1);
        } break;
        case WM_COMMAND:

            switch (HIWORD(wParam)) {

                case 0:
                    switch (LOWORD(wParam)) {

                        case GUI_MENU_NEW: {
                            new_file();
                        } break;
                        case GUI_MENU_SAVE: {

                            PCWSTR fname = choose_file(TRUE);

                            save_to_file(fname);
                        } break;
                        case GUI_MENU_LOAD: {

                            // Prompt the user to choose a file
                            PCWSTR fname = choose_file(FALSE);
                            // Load from file to control
                            load_from_file(fname);
                        } break;
                        case GUI_MENU_WWRAP: {
                            toggle_wwrap();
                        } break;
                        case GUI_MENU_ABOUT: 
                            MessageBoxW(
                                Window, 
                                L"This application is public domain, the source code is publicly available at github.com/jacobsebek/JTEdit\n"
                                L"There is no warranty, use at own risk of losing your files.",
                                L"About",
                                MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                break;
            }
        break;
        default:
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return 0;
}

// The entry point of the program, this function creates the main window and starts the message loop
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nShowCmd) {

    // This procedure is necessary to ensure that up-to-date controls get loaded
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_STANDARD_CLASSES;
    // It is not crucial for this to succeed, so we don't care if it fails (it will just use the old style)
    InitCommonControlsEx(&icc);

    // Setup the main window
    {
        // Specify the style
        PCWSTR class = L"MainClass";
        PCWSTR title = L"Jittey 0.1";
        DWORD window_style = WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
        HBRUSH bgcol = GetSysColorBrush(COLOR_WINDOW);

        // Register the main window class
        {
            WNDCLASSEXW wc = {0};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpszClassName = class;
            wc.hInstance = hInstance;
            wc.lpfnWndProc = WndProc;
            wc.hbrBackground = bgcol;
            wc.hIcon =   LoadImageW(hInstance, L"myIcon", IMAGE_ICON, 24, 24, LR_DEFAULTCOLOR);
            wc.hIconSm = LoadImageW(hInstance, L"myIcon", IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            //wc.hbrBackground = CreateSolidBrush(RGB(220, 220, 220));
          
            if (!RegisterClassExW(&wc))
                fatal(L"Failed to register the main class");
        }

        // First, calculate the size of the whole window based on the client area size
        INT wW, wH;
        {
            RECT wrect = {0, 0, Width, Height};
            AdjustWindowRect(&wrect, window_style, TRUE);

            wW = wrect.right-wrect.left;
            wH = wrect.bottom-wrect.top;
        }

        CreateWindowW( class, 
                       title,
                       window_style,
                       CW_USEDEFAULT, CW_USEDEFAULT, 
                       wW, wH,
                       NULL, NULL, hInstance, NULL);

        if (!Window)
            fatal(L"Failed to create the main window");
    }

    // Open the specified file in the console, if any
    // When a file is "opened with" this app, the full command line looks like this:
    // "path/to/the/app" "path/to/the/file"
    // Luckily, we can use the CommandLIneToArgv function that does all the parsing
    {
        INT argc;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        if (argv != NULL && argc > 1) {
            // Open the file (it handles the NULL argument case)
            load_from_file(argv[1]);
        }

        LocalFree(argv);
    }

    // This thing, this thing...
    // If acctable has one element, the CreateAcceleratorTableW function fails under GCC, why? I have no idea.
    //TODO: find out why this fails, even though it's not that big of a concern
    // Setup an accelerator table for the edit box
    // This could also be achieved by catching a EM_CHAR for the character that gets emmited when we press Ctrl+Backspace I suppose
    ACCEL acctable[2] = {
        {.fVirt = FCONTROL | FVIRTKEY, .key = VK_BACK, .cmd = ACC_EDIT_DELETEWORD},
        {0,0,0}
    };

    Gui.edit_accels = CreateAcceleratorTableW(acctable, 1);
    if (!Gui.edit_accels)
        fatal(L"Failed to create the accelerator table");

    MSG msg;
    BOOL stat;
    while (stat = GetMessageW(&msg, NULL, 0, 0)) {

        if (stat == -1)
            fatal(L"GetMessage error");

        // Make sure that the accelerators are invoked only if the text box has keyboard focus
        if (GetFocus() != Gui.text_box || !TranslateAcceleratorW(Gui.text_box, Gui.edit_accels, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return 0;
}
