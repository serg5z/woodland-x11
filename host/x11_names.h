/* Core X11 request names by major opcode (protocol specification, encoding appendix). */
#ifndef X11_NAMES_H
#define X11_NAMES_H
#include <stdint.h>

static const char *const X11_REQUEST_NAMES[128] = {
    0, "CreateWindow", "ChangeWindowAttributes", "GetWindowAttributes", "DestroyWindow", "DestroySubwindows",
    "ChangeSaveSet", "ReparentWindow", "MapWindow", "MapSubwindows", "UnmapWindow", "UnmapSubwindows",
    "ConfigureWindow", "CirculateWindow", "GetGeometry", "QueryTree", "InternAtom", "GetAtomName",
    "ChangeProperty", "DeleteProperty", "GetProperty", "ListProperties", "SetSelectionOwner",
    "GetSelectionOwner", "ConvertSelection", "SendEvent", "GrabPointer", "UngrabPointer", "GrabButton",
    "UngrabButton", "ChangeActivePointerGrab", "GrabKeyboard", "UngrabKeyboard", "GrabKey", "UngrabKey",
    "AllowEvents", "GrabServer", "UngrabServer", "QueryPointer", "GetMotionEvents", "TranslateCoordinates",
    "WarpPointer", "SetInputFocus", "GetInputFocus", "QueryKeymap", "OpenFont", "CloseFont", "QueryFont",
    "QueryTextExtents", "ListFonts", "ListFontsWithInfo", "SetFontPath", "GetFontPath", "CreatePixmap",
    "FreePixmap", "CreateGC", "ChangeGC", "CopyGC", "SetDashes", "SetClipRectangles", "FreeGC", "ClearArea",
    "CopyArea", "CopyPlane", "PolyPoint", "PolyLine", "PolySegment", "PolyRectangle", "PolyArc", "FillPoly",
    "PolyFillRectangle", "PolyFillArc", "PutImage", "GetImage", "PolyText8", "PolyText16", "ImageText8",
    "ImageText16", "CreateColormap", "FreeColormap", "CopyColormapAndFree", "InstallColormap",
    "UninstallColormap", "ListInstalledColormaps", "AllocColor", "AllocNamedColor", "AllocColorCells",
    "AllocColorPlanes", "FreeColors", "StoreColors", "StoreNamedColor", "QueryColors", "LookupColor",
    "CreateCursor", "CreateGlyphCursor", "FreeCursor", "RecolorCursor", "QueryBestSize", "QueryExtension",
    "ListExtensions", "ChangeKeyboardMapping", "GetKeyboardMapping", "ChangeKeyboardControl",
    "GetKeyboardControl", "Bell", "ChangePointerControl", "GetPointerControl", "SetScreenSaver",
    "GetScreenSaver", "ChangeHosts", "ListHosts", "SetAccessControl", "SetCloseDownMode", "KillClient",
    "RotateProperties", "ForceScreenSaver", "SetPointerMapping", "GetPointerMapping", "SetModifierMapping",
    "GetModifierMapping", 0, 0, 0, 0, 0, 0, 0, "NoOperation"};

static const char *const X11_EVENT_NAMES[35] = {
    0, 0, "KeyPress", "KeyRelease", "ButtonPress", "ButtonRelease", "MotionNotify", "EnterNotify",
    "LeaveNotify", "FocusIn", "FocusOut", "KeymapNotify", "Expose", "GraphicsExpose", "NoExpose",
    "VisibilityNotify", "CreateNotify", "DestroyNotify", "UnmapNotify", "MapNotify", "MapRequest",
    "ReparentNotify", "ConfigureNotify", "ConfigureRequest", "GravityNotify", "ResizeRequest",
    "CirculateNotify", "CirculateRequest", "PropertyNotify", "SelectionClear", "SelectionRequest",
    "SelectionNotify", "ColormapNotify", "ClientMessage", "MappingNotify"};

static inline const char *x11_event_name(uint8_t code) {
  code &= 0x7F;  /* sent-event flag */
  return code < 35 && X11_EVENT_NAMES[code] ? X11_EVENT_NAMES[code] : "unknown";
}

static inline const char *x11_request_name(uint8_t op) {
  if (op >= 128) return "extension";
  return X11_REQUEST_NAMES[op] ? X11_REQUEST_NAMES[op] : "unknown";
}
#endif
