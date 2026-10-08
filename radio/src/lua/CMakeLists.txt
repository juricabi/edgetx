
add_definitions(-DLUA)

if(LUA_COMPILER)
  add_definitions(-DLUA_COMPILER)
endif()

if(LUA_ALLOCATOR_TRACER AND DEBUG)
  add_definitions(-DLUA_ALLOCATOR_TRACER)
endif()

if(NOT "${LUA_SCRIPT_LOAD_MODE}" STREQUAL "")
  add_definitions(-DLUA_SCRIPT_LOAD_MODE="${LUA_SCRIPT_LOAD_MODE}")
endif()

include_directories(${LUA_DIR})

if(LUA_MIXER)
  add_definitions(-DLUA_MODEL_SCRIPTS)
endif()

set(SRC ${SRC}
  lua/interface.cpp
  lua/api_general.cpp
  lua/api_model.cpp
  lua/api_filesystem.cpp
  lua/lua_event.cpp
)

if(GUI_DIR STREQUAL colorlcd)
  set(SRC ${SRC} lua/api_colorlcd.cpp lua/api_colorlcd_lvgl.cpp lua/widgets.cpp)
else()
  set(SRC ${SRC} lua/api_stdlcd.cpp)
endif()

set(LUA_SRC
  lapi.c
  lcode.c
  lctype.c
  ldebug.c
  ldo.c
  ldump.c
  lfunc.c
  lgc.c
  llex.c
  lmem.c
  lobject.c
  lopcodes.c
  lparser.c
  lstate.c
  lstring.c
  ltable.c
  ltm.c
  lundump.c
  lvm.c
  lzio.c
  linit.c
  lbaselib.c
  lmathlib.c
  lbitlib.c
  loadlib.c
  lauxlib.c
  ltablib.c
  lcorolib.c
  liolib.c
  lstrlib.c
)

add_compile_definitions(LUA_COMPAT_5_2)

foreach(FILE ${LUA_SRC})
  set(SRC ${SRC} ${LUA_DIR}/${FILE})
endforeach()

# StickTime build: the STM32H750 radios (every target linked with stm32h750_sdram) run their
# firmware from SDRAM and leave the 64 KB ITCM empty. With LUA_FAST_CODE_ITCM the functions
# marked LUAI_FASTCODE run from ITCM instead, and the files that hold the interpreter's hot
# paths are compiled for speed instead of size.
if(TARGET_LINKER_DIR STREQUAL "stm32h750_sdram")
  set(LUA_FAST_CODE_ITCM ON)
endif()
if(LUA_FAST_CODE_ITCM)
  add_definitions(-DLUA_FAST_CODE_ITCM)
  foreach(FILE lapi.c ldo.c lfunc.c lgc.c lmem.c lobject.c lstate.c lstring.c ltable.c ltm.c
               lvm.c lbaselib.c lmathlib.c lauxlib.c ltablib.c)
    set_source_files_properties(${LUA_DIR}/${FILE} PROPERTIES COMPILE_FLAGS -O2)
  endforeach()
endif()
