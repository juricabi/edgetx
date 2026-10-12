/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "standalone_lua.h"

#include "dma2d.h"
#include "lcd.h"
#include "edgetx.h"
#include "timers_driver.h"
#include "keys.h"
#include "lua/lua_event.h"
#include "view_main.h"

#if defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

lua_State *lsStandalone = nullptr;

#if defined(LUA_ALLOCATOR_TRACER)
LuaMemTracer lsStandaloneTrace;

static void luaStandaloneHook(lua_State * L, lua_Debug *ar)
{
  if (ar->event == LUA_HOOKLINE) {
    lua_getinfo(L, "nSl", ar);
    LuaMemTracer * tracer = &lsStandaloneTrace;
    if (tracer->alloc || tracer->free) {
      TRACE("LT: [+%u,-%u] %s:%d", tracer->alloc, tracer->free, tracer->script, tracer->lineno);
    }
    tracer->script = ar->source;
    tracer->lineno = ar->currentline;
    tracer->alloc = 0;
    tracer->free = 0;
  }
}
#endif // #if defined(LUA_ALLOCATOR_TRACER)

static void luaStandaloneInit()
{
#if defined(USE_CUSTOM_ALLOCATOR)
  lsStandalone = lua_newstate(custom_l_alloc, nullptr);   //we use our own allocator!
#elif defined(LUA_ALLOCATOR_TRACER)
  memclear(&lsStandaloneTrace, sizeof(lsStandaloneTrace));
  lsStandaloneTrace.script = "lua_newstate(scripts)";
  lsStandalone = lua_newstate(tracer_alloc, &lsStandaloneTrace);   //we use tracer allocator
#else
  lsStandalone = luaL_newstate();   //we use Lua default allocator
#endif
  if (lsStandalone) {
    // install our panic handler
    extern int custom_lua_atpanic(lua_State *L);
    lua_atpanic(lsStandalone, &custom_lua_atpanic);

#if defined(LUA_ALLOCATOR_TRACER)
    lua_sethook(lsStandalone, luaStandaloneHook, LUA_MASKLINE);
#endif

    // protect libs and constants registration
    PROTECT_LUA() {
      luaRegisterLibraries(lsStandalone);
    } else {
      luaClose(&lsStandalone);
    }
    UNPROTECT_LUA();
  }
}

void luaExecStandalone(const char * filename)
{
  if (lsStandalone == nullptr)
    luaStandaloneInit();

  PROTECT_LUA() {
    if (luaLoadScriptFileToState(lsStandalone, filename, LUA_SCRIPT_LOAD_MODE) == SCRIPT_OK) {
      if (lua_pcall(lsStandalone, 0, 1, 0) == LUA_OK && lua_istable(lsStandalone, -1)) {
        int initFunction = LUA_REFNIL, runFunction = LUA_REFNIL;
        bool lvglLayout = false;

        for (lua_pushnil(lsStandalone); lua_next(lsStandalone, -2); lua_pop(lsStandalone, 1)) {
          const char * key = lua_tostring(lsStandalone, -2);
          if (!strcmp(key, "init")) {
            initFunction = luaL_ref(lsStandalone, LUA_REGISTRYINDEX);
            lua_pushnil(lsStandalone);
          } else if (!strcmp(key, "run")) {
            runFunction = luaL_ref(lsStandalone, LUA_REGISTRYINDEX);
            lua_pushnil(lsStandalone);
          } else if (!strcasecmp(key, "useLvgl")) {
            lvglLayout = lua_toboolean(lsStandalone, -1);
          }
        }

        StandaloneLuaWindow::setup(lvglLayout, initFunction, runFunction);
      }
      else {
        TRACE("luaLoadFile(%s): Error parsing script: %s", filename, lua_tostring(lsStandalone, -1));
      }
    }
  }
  else {
    // error while loading Lua widget/theme,
    // do not disable whole Lua state, just ingnore bad script
    return;
  }
  UNPROTECT_LUA();
}

// singleton instance
StandaloneLuaWindow* StandaloneLuaWindow::_instance;

// StickTime build: tells the UI task to run its loop faster while a Lua tool is open
bool standaloneLuaActive() { return StandaloneLuaWindow::instance() != nullptr; }

// StickTime build: how long the tool's last run() took, and how long putting that frame on
// the screen took (waiting for the display, then LVGL drawing it, or handing the frame to the
// display), in microseconds. The tool reads them as the globals TOOL_RUN_US and TOOL_SHOW_US.
static uint32_t toolRunUs = 0, toolShowUs = 0;

// a whole frame, rounded up to 64 bytes (the frames start on 64 byte boundaries)
static constexpr size_t FRAME_STRIDE =
    ((size_t)LCD_W * LCD_H * sizeof(uint16_t) + 63) & ~(size_t)63;

void StandaloneLuaWindow::frameStart()
{
  // the tool draws into a frame that is neither on the display nor on its way there
  uint16_t* f = frames[frameNext];
  lcdBuffer->setData(f);
  if (frameCanvas) lv_canvas_set_buffer(frameCanvas, f, LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
  luaLcdKeepFrame = frameShown;
  luaLcdDrawn = false;
}

void StandaloneLuaWindow::endDirectFrames()
{
  if (!framesDirect) return;
  framesDirect = false;
  lv_disp_enable_invalidation(lv_disp_get_default(), true);
  // LVGL's frame buffers hold an old picture: the whole screen is drawn again
  lv_obj_invalidate(lv_scr_act());
  lv_obj_invalidate(lv_layer_top());
}

// anything EdgeTX shows over the tool: a dialog (a layer of its own), a message bubble (LVGL's
// top layer), or a window put over it (the power off screen)
bool StandaloneLuaWindow::othersOnTop()
{
  if (hasError || Window::topWindow() != this || !lvobj) return true;
  if (lv_obj_get_child_cnt(lv_layer_top()) > 0) return true;
  // LVGL objects the tool made on its own window (lvgl.* without useLvgl), next to the canvas
  for (uint32_t i = 0, n = lv_obj_get_child_cnt(lvobj); i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(lvobj, i);
    if (c != frameCanvas && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) return true;
  }
  lv_obj_t* parent = lv_obj_get_parent(lvobj);
  if (!parent) return false;
  for (int32_t i = (int32_t)lv_obj_get_child_cnt(parent) - 1; i >= 0; i--) {
    lv_obj_t* c = lv_obj_get_child(parent, i);
    if (c == lvobj) return false;
    if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) return true;
  }
  return false;
}

void StandaloneLuaWindow::checkDirectFrames()
{
  if (framesDirect && othersOnTop()) {
    // between frames: the one being drawn next holds an older picture
    lcdBuffer->setData(frameShown);
    if (frameCanvas)
      lv_canvas_set_buffer(frameCanvas, frameShown, LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
    endDirectFrames();
  }
}

// StickTime build: called whenever the UI runs (MainWindow::run(): the UI loop, the power off
// screen, dialogs that wait for an answer). EdgeTX may show something over a tool whose frames
// are on the display, and then not run the tool at all (a dialog is the window it runs)
void standaloneLuaCheckDirect()
{
  auto w = StandaloneLuaWindow::instance();
  if (w) w->checkDirectFrames();
}

void StandaloneLuaWindow::frameShow()
{
  bool drawn = luaLcdDrawn;
  luaLcdKeepFrame = nullptr;
  luaLcdDrawn = true;

  // straight to the display, unless EdgeTX shows something over the tool
  bool direct = !othersOnTop();

  if (!drawn) {
    // nothing drawn: the last frame stays (this one holds an older picture)
    if (direct && framesDirect) return;
    endDirectFrames();
    lcdBuffer->setData(frameShown);
    if (frameCanvas)
      lv_canvas_set_buffer(frameCanvas, frameShown, LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
    invalidate();
    lv_refr_now(nullptr);
    return;
  }

  uint16_t* f = frames[frameNext];
  if (direct) {
    if (!framesDirect) {
      // LVGL leaves the screen alone while the tool's frames are on it: nothing it has to
      // draw again (LVGL's refresh only draws what was invalidated, and any invalidation
      // would also restart it; the canvas is invalidated by every text the tool draws)
      lv_disp_t* disp = lv_disp_get_default();
      _lv_inv_area(disp, nullptr);
      lv_disp_enable_invalidation(disp, false);
      framesDirect = true;
    }
    lcdDirectFrameShow(f);
  } else {
    endDirectFrames();
    invalidate();
    lv_refr_now(nullptr);
  }
  frameShown = f;
  frameNext = (frameNext + 1) % 3;
}

StandaloneLuaWindow::StandaloneLuaWindow(bool useLvgl, int initFn, int runFn) :
    Window(MainWindow::instance(), {0, 0, LCD_W, LCD_H}),
    useLvgl(useLvgl), initFunction(initFn), runFunction(runFn)
{
  setWindowFlag(OPAQUE);

  etx_solid_bg(lvobj);

  luaScriptManager = this;

  pushLayer(true);

  MainWindow::instance()->enableWidgetRefresh(false);

  if (useLvglLayout()) {
    padAll(PAD_ZERO);
    etx_scrollbar(lvobj);

    lv_obj_t* lbl = etx_label_create(lvobj, FONT_XL_INDEX);
    lv_obj_set_pos(lbl, 0, 0);
    lv_obj_set_size(lbl, LCD_W, LCD_H);
    etx_solid_bg(lbl, COLOR_THEME_PRIMARY1_INDEX);
    etx_txt_color(lbl, COLOR_THEME_PRIMARY2_INDEX);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, (LCD_H - EdgeTxStyles::STD_FONT_HEIGHT) / 2, LV_PART_MAIN);
    lv_label_set_text(lbl, STR_LOADING);
  } else {
    // three frames instead of one: only where at least 1 MB stays free (the H750 radios with
    // 8 MB of SDRAM have 2-3 MB of heap)
    if (lcdDirectFramesSupported() &&
        availableMemory() > 3 * FRAME_STRIDE + 64 + 1024 * 1024) {
      framesMem = malloc(3 * FRAME_STRIDE + 64);
      if (framesMem) {
        auto p = (uint8_t*)(((uintptr_t)framesMem + 63) & ~(uintptr_t)63);
        for (int i = 0; i < 3; i++) frames[i] = (uint16_t*)(p + i * FRAME_STRIDE);
        frameShown = frames[0];
        frameNext = 1;
      }
    }
    lcdBuffer = framesMem ? new BitmapBuffer(BMP_RGB565, LCD_W, LCD_H, frames[0])
                          : new BitmapBuffer(BMP_RGB565, LCD_W, LCD_H);
    frameCanvas = lcdBuffer->addCanvas(this);
    lcdBuffer->setFastDraw(true);

    lcdBuffer->clear();
    lcdBuffer->drawText(LCD_W / 2, LCD_H / 2 - EdgeTxStyles::STD_FONT_HEIGHT, STR_LOADING,
                      FONT(L) | COLOR_THEME_PRIMARY2 | CENTERED);
    setWindowFlag(NO_FOCUS | NO_SCROLL);

    lv_group_add_obj(lv_group_get_default(), lvobj);
    lv_group_set_editing(lv_group_get_default(), true);
  }

  // setup LUA event handler
  setupHandler(this);

  lua_gc(lsStandalone, LUA_GCCOLLECT, 0);

  // Pause function and mixer scripts
  prevLuaState = luaState;
  luaState = INTERPRETER_PAUSED;

#if defined(USE_HATS_AS_KEYS)
  setTransposeHatsForLUA(true);
#endif

  onClosing([=]() {
    // In case keyboard was open when window closed
    Keyboard::hideKeyboard();

    if (framesMem) {
      // the display back on LVGL's frame buffers before the tool's frames go
      endDirectFrames();
      if (frameCanvas) lv_obj_add_flag(frameCanvas, LV_OBJ_FLAG_HIDDEN);
      lv_refr_now(nullptr);
      lcdDirectFrameWait();
      luaLcdKeepFrame = nullptr;
      luaLcdDrawn = true;
    }

    luaL_unref(lsStandalone, LUA_REGISTRYINDEX, initFunction);
    luaL_unref(lsStandalone, LUA_REGISTRYINDEX, runFunction);
    luaLcdBuffer = nullptr;

    luaClose(&lsStandalone);

    if (lcdBuffer) delete lcdBuffer;
    lcdBuffer = nullptr;
    if (framesMem) free(framesMem);
    framesMem = nullptr;

    luaScriptManager = nullptr;

    _instance = nullptr;

#if defined(USE_HATS_AS_KEYS)
    setTransposeHatsForLUA(false);
#endif

    luaState = prevLuaState;

    luaEmptyEventBuffer();

    MainWindow::instance()->enableWidgetRefresh(true);
  });
}

void StandaloneLuaWindow::setup(bool useLvgl, int initFn, int runFn)
{
  if (_instance == nullptr)
    _instance = new StandaloneLuaWindow(useLvgl, initFn, runFn);
}

StandaloneLuaWindow* StandaloneLuaWindow::instance()
{
  return _instance;
}

void StandaloneLuaWindow::checkEvents()
{
  Window::checkEvents();

  if (initFunction != LUA_REFNIL) {
    lua_rawgeti(lsStandalone, LUA_REGISTRYINDEX, initFunction);
    if (lua_pcall(lsStandalone, 0, 0, 0) != LUA_OK) {
      luaShowError();
    }
    luaL_unref(lsStandalone, LUA_REGISTRYINDEX, initFunction);
    initFunction = LUA_REFNIL;
    return;
  }

  // Set global LUA LCD buffer
  luaLcdBuffer = lcdBuffer;
  luaLcdAllowed = !useLvglLayout();

  LuaEventData evt;
  // Pull a new event from the buffer
  luaNextEvent(&evt);
  if (evt.event == EVT_KEY_LONG(KEY_EXIT)) {
    killEvents(evt.event);
    closeWindow();
  } else {
    if (runFunction != LUA_REFNIL) {
      lua_pushinteger(lsStandalone, toolRunUs);
      lua_setglobal(lsStandalone, "TOOL_RUN_US");
      lua_pushinteger(lsStandalone, toolShowUs);
      lua_setglobal(lsStandalone, "TOOL_SHOW_US");
      uint32_t t0 = timersGetUsTick();
      if (framesMem && !useLvglLayout()) frameStart();
      lua_rawgeti(lsStandalone, LUA_REGISTRYINDEX, runFunction);

      lua_pushunsigned(lsStandalone, evt.event);
      int inputsCount = 1;

#if defined(HARDWARE_TOUCH)
      if (IS_TOUCH_EVENT(evt.event)) {
        luaPushTouchEventTable(lsStandalone, &evt);
        inputsCount += 1;
      }
#endif

      if (lua_pcall(lsStandalone, inputsCount, 1, 0) != LUA_OK) {
        luaShowError();
      } else {
        if (lua_isnumber(lsStandalone, -1)) {
          int scriptResult = lua_tointeger(lsStandalone, -1);
          lua_pop(lsStandalone, 1);  /* pop returned value */
          if (scriptResult != 0) {
            closeWindow();
          } else {
            if (useLvglLayout() && !hasError) {
              PROTECT_LUA() {
                if (!callRefs(lsStandalone)) {
                  luaShowError();
                }
              } else {
                luaShowError();
              }
              UNPROTECT_LUA();
            } else {
              // StickTime build: show the frame now, not at the next UI cycle
              uint32_t t1 = timersGetUsTick();
              if (framesMem) {
                frameShow();
              } else {
                invalidate();
                lv_refr_now(nullptr);
              }
              toolRunUs = t1 - t0;
              toolShowUs = timersGetUsTick() - t1;
            }
          }
        } else if (lua_isstring(lsStandalone, -1)) {
          char nextScript[FF_MAX_LFN+1];
          strncpy(nextScript, lua_tostring(lsStandalone, -1), FF_MAX_LFN);
          nextScript[FF_MAX_LFN] = '\0';
          _instance = nullptr;
          lua_settop(lsStandalone, 0);
          closeWindow();
          luaExecStandalone(nextScript);
        } else if (framesMem && !useLvglLayout()) {
          // StickTime build: a run() that returns no number (nil) goes on, and so does its frame
          lua_pop(lsStandalone, 1);
          frameShow();
        }
      }
    }
  }

  // Kill global LUA LCD buffer
  luaLcdBuffer = nullptr;
  luaLcdAllowed = false;
}

void StandaloneLuaWindow::onClicked() { LuaScriptManager::onClickedEvent(); }

void StandaloneLuaWindow::onCancel() { LuaScriptManager::onCancelEvent(); }

void StandaloneLuaWindow::onEvent(event_t evt)
{
  LuaScriptManager::onLuaEvent(evt);
}

void StandaloneLuaWindow::popupPaint(BitmapBuffer* dc, coord_t x, coord_t y, coord_t w, coord_t h,
                                     const char* text, const char* info)
{
  // popup background
  dc->drawSolidFilledRect(x, y, w, POPUP_HEADER_HEIGHT, COLOR_THEME_FOCUS);

  // title bar
  dc->drawText(x + PAD_SMALL,
               y + (POPUP_HEADER_HEIGHT - getFontHeight(FONT(STD))) / 2, text,
               COLOR_THEME_PRIMARY2);

  dc->drawSolidFilledRect(x, y + POPUP_HEADER_HEIGHT, w,
                          h - POPUP_HEADER_HEIGHT, COLOR_THEME_SECONDARY3);

  dc->drawText(x + PAD_SMALL, y + POPUP_HEADER_HEIGHT + EdgeTxStyles::STD_FONT_HEIGHT, info,
               COLOR_THEME_SECONDARY1);
}

bool StandaloneLuaWindow::displayPopup(event_t event, uint8_t type,
                                       const char* text, const char* info,
                                       bool& result)
{
  if (useLvgl) return true;

  // over the last frame (see luaLcdKeepFrame)
  luaLcdTouch();

  // transparent background
  lcdBuffer->drawFilledRect(0, 0, LCD_W, LCD_H, SOLID, COLOR_THEME_PRIMARY1,
                            OPACITY(5));

  popupPaint(lcdBuffer, POPUP_X, POPUP_Y, LCD_W - POPUP_X * 2, LCD_H - POPUP_Y * 2, text, info);

  // TRACE("displayPopup(event = 0x%x)", event);
  if (event == EVT_KEY_BREAK(KEY_EXIT)) {
    result = false;
    return true;
  } else if (event == EVT_KEY_BREAK(KEY_ENTER)) {
    result = true;
    return true;
  }

  return false;
}

void StandaloneLuaWindow::clear()
{
  clearRefs(lsStandalone);
  // the canvas would go with the window's other LVGL objects: out first, then back
  if (lcdBuffer) lcdBuffer->removeCanvas();
  frameCanvas = nullptr;
  Window::clear();
  if (lcdBuffer) frameCanvas = lcdBuffer->addCanvas(this);
}

void StandaloneLuaWindow::luaShowError()
{
  if (runFunction != LUA_REFNIL) luaL_unref(lsStandalone, LUA_REGISTRYINDEX, runFunction);
  runFunction = LUA_REFNIL;
  hasError = true;
  extern void luaError(lua_State *L, uint8_t error);
  luaError(lsStandalone, SCRIPT_SYNTAX_ERROR);
}

void StandaloneLuaWindow::showError(bool firstCall, const char* title, const char* msg)
{
  runFunction = LUA_REFNIL;
  hasError = true;
  if (framesMem) {
    // LVGL shows the error, over the last finished frame
    endDirectFrames();
    luaLcdKeepFrame = nullptr;
    luaLcdDrawn = true;
    lcdBuffer->setData(frameShown);
    if (frameCanvas)
      lv_canvas_set_buffer(frameCanvas, frameShown, LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
  }
  if (!errorModal) {
    lv_obj_set_scroll_dir(lvobj, LV_DIR_NONE);
    errorModal = lv_obj_create(lvobj);
    lv_obj_set_pos(errorModal, lv_obj_get_scroll_x(lvobj), lv_obj_get_scroll_y(lvobj));
    lv_obj_set_size(errorModal, LCD_W, LCD_H);
    etx_bg_color(errorModal, COLOR_BLACK_INDEX);
    etx_obj_add_style(errorModal, styles->bg_opacity_75, LV_PART_MAIN);
    errorTitle = etx_label_create(errorModal, FONT_L_INDEX);
    lv_obj_set_pos(errorTitle, ERR_TTL_X, ERR_TTL_Y);
    lv_obj_set_size(errorTitle, LCD_W - ERR_TTL_X * 2, EdgeTxStyles::UI_ELEMENT_HEIGHT);
    etx_txt_color(errorTitle, COLOR_THEME_PRIMARY2_INDEX);
    etx_solid_bg(errorTitle, COLOR_THEME_SECONDARY1_INDEX);
    etx_obj_add_style(errorTitle, styles->text_align_center, LV_PART_MAIN);
    errorMsg = etx_label_create(errorModal);
    lv_obj_set_pos(errorMsg, ERR_TTL_X, ERR_MSG_Y);
    lv_obj_set_size(errorMsg, LCD_W - ERR_TTL_X * 2, LCD_H - ERR_MSG_HO);
    lv_obj_set_style_pad_all(errorMsg, PAD_SMALL, LV_PART_MAIN);
    etx_txt_color(errorMsg, COLOR_THEME_PRIMARY1_INDEX);
    etx_solid_bg(errorMsg, COLOR_THEME_SECONDARY3_INDEX);
    etx_obj_add_style(errorMsg, styles->text_align_center, LV_PART_MAIN);
  }

  lv_label_set_text(errorTitle, title);
  lv_label_set_text(errorMsg, msg);
}
