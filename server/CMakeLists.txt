# Builds AvP2's server game DLL (object.lto) from Monolith's "AvP2 Source Code
# v1.0.9.6" with Visual Studio 2022, as a Win32 Release DLL for the retail
# LithTech Talon engine (AvP2 1.0.9.6).
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release
#
# The Monolith sources live in game/ (a local-only git repository: pristine
# import first, then one commit per fix). This file, compat/ and build.bat
# are ours.
cmake_minimum_required(VERSION 3.20)
project(avp2_object LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "object.lto must be 32-bit: configure with -A Win32")
endif()

set(GAME "${CMAKE_SOURCE_DIR}/game")
set(AVP2 "${GAME}/AVP2")
set(LS   "${GAME}/LT2/lithshared")       # Monolith's headers (and WONAPI source)
set(LSS  "${GAME}/LT2/lithshared_src")   # library sources (see PROVENANCE.txt)
set(SDK  "${GAME}/LT2/sdk/inc")

# --- ABI / code generation, shared by every target --------------------------
# Retail object.lto: VC6 SP3, /MT /GR /GX /O2, default packing (/Zp8), __cdecl.
# All of those are still MSVC's defaults; what we add:
#   static CRT (/MT)      as retail (it imports no CRT DLL)
#   /arch:IA32            x87 floating point like VC6, not SSE2
#   /Zc:forScope-         VC6 for-loop variable scoping, which the code relies on
#   /Zc:wchar_t-          wchar_t = unsigned short, as in VC6 (WONAPI needs it)
#   /permissive           (no /permissive-) accept VC6-era constructs
#   /GS- /Gy              no behaviour change; no security cookies, as VC6
#   NOMINMAX              the code calls std::min/std::max; keep <windows.h> macros out
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W3 /GR /EHsc /MP /arch:IA32 /Zc:forScope- /Zc:wchar_t- /GS- /Gy
                    /wd4996 /wd4244 /wd4305 /wd4018 /wd4267 /wd4838 /wd4091 /wd4477 /wd4288)
add_compile_definitions(WIN32 _WINDOWS
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _CRT_NON_CONFORMING_SWPRINTFS
    _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    _HAS_AUTO_PTR_ETC=1 NOMINMAX
    NO_PRAGMA_LIBS)
# compat/ supplies the pre-standard headers the code includes: <iostream.h>,
# <fstream.h>, <strstrea.h>, and STLport's <hash_map>/<hash_set>. The hash
# containers reproduce STLport's iteration order exactly, which the game's
# network ids depend on (see compat/stlport_hashtable.h).
include_directories(BEFORE "${CMAKE_SOURCE_DIR}/compat")

# --- lithshared static libraries ---------------------------------------------
add_library(StdLith STATIC
    ${LSS}/stdlith/abstractio.cpp ${LSS}/stdlith/dynarray.cpp ${LSS}/stdlith/fileio.cpp
    ${LSS}/stdlith/goodlinklist.cpp ${LSS}/stdlith/helpers.cpp ${LSS}/stdlith/l_allocator.cpp
    ${LSS}/stdlith/linklist.cpp ${LSS}/stdlith/lithexception.cpp ${LSS}/stdlith/memory.cpp
    ${LSS}/stdlith/memoryio.cpp ${LSS}/stdlith/stringholder.cpp ${LSS}/stdlith/struct_bank.cpp)
target_include_directories(StdLith PRIVATE ${LS}/stdlith ${LS}/lith ${LSS}/stdlith ${LS}/incs)
target_compile_definitions(StdLith PRIVATE NDEBUG _NOMFC)

add_library(MFCStub STATIC
    ${LSS}/MFCStub/mfcs_string.cpp ${LSS}/MFCStub/mfcs_rect.cpp ${LSS}/MFCStub/mfcs_point.cpp)
target_include_directories(MFCStub PRIVATE ${LS}/mfcstub ${LS}/incs ${LS}/lith ${LSS}/MFCStub)
target_compile_definitions(MFCStub PRIVATE NDEBUG _NOMFC)

add_library(CryptMgr STATIC ${LSS}/CryptMgr/cryptmgr.cpp ${LSS}/CryptMgr/blowfish.cpp)
target_include_directories(CryptMgr PRIVATE ${LS}/cryptmgr ${LS}/incs)
target_compile_definitions(CryptMgr PRIVATE NDEBUG)

add_library(ButeMgrNoMFC STATIC
    ${LSS}/ButeMgr/arange.cpp ${LSS}/ButeMgr/avector.cpp ${LSS}/ButeMgr/butemgr.cpp ${LSS}/ButeMgr/stdafx.cpp)
target_include_directories(ButeMgrNoMFC PRIVATE ${LS}/butemgr ${LS}/stdlith ${LS}/mfcstub
    ${LS}/cryptmgr ${LS}/lith ${LS}/incs)
target_compile_definitions(ButeMgrNoMFC PRIVATE NDEBUG _NOMFC)

add_library(GameSpyMgr STATIC ${LSS}/gamespymgr/GameSpyMgr.cpp ${LSS}/gamespymgr/gsutils.cpp)
target_include_directories(GameSpyMgr PRIVATE ${LS}/gamespy/gamespymgr ${LS}/gamespy/cengine
    ${LS}/gamespy ${LS}/wonapi ${LS}/incs ${LS}/mfcstub)
target_compile_definitions(GameSpyMgr PRIVATE NDEBUG _NOMFC)

# WONAPI (Sierra's WON client; GameSpyMgr registers the server with it). Built the
# way WONAPI.dsp does: only the "master" .cpp of each module, which #include the rest.
set(WON "${LS}/wonapi")
add_library(WONAPI STATIC
    ${WON}/WONAPI.cpp ${WON}/WONStatus.cpp
    ${WON}/WONAuth/Auth.cpp ${WON}/WONCommon/Common.cpp ${WON}/WONCrypt/Crypt.cpp
    ${WON}/WONDir/Dir.cpp ${WON}/WONMisc/Misc.cpp ${WON}/WONServer/Server.cpp
    ${WON}/WONSocket/Socket.cpp)
target_include_directories(WONAPI PRIVATE ${WON})
target_compile_definitions(WONAPI PRIVATE NDEBUG _MBCS _LIB __WON_MASTER_CPP_BUILD__)

# --- object.lto ---------------------------------------------------------------
# Source list = every .cpp of Monolith's ObjectDLL/Object.dsp ("Win32 Final").
file(STRINGS "${CMAKE_SOURCE_DIR}/object_sources.txt" OBJECT_SOURCES)
list(TRANSFORM OBJECT_SOURCES PREPEND "${AVP2}/")

add_library(object SHARED ${OBJECT_SOURCES})
# --- avp2bots: our own code (open-source part), kept outside game/ ----------
if(NOT DEFINED BOTS)
    set(BOTS "${CMAKE_SOURCE_DIR}/../bots")
endif()
target_sources(object PRIVATE ${BOTS}/CrashLog.cpp ${BOTS}/BotNav.cpp ${BOTS}/BotMgr.cpp ${BOTS}/BotRoamGoal.cpp ${BOTS}/MspClient.cpp)
# A server's own extensions (ServerEvents.cpp, BotCamera.cpp: not published) when present, else no-op stubs
if(EXISTS "${BOTS}/ServerEvents.cpp")
    target_sources(object PRIVATE ${BOTS}/ServerEvents.cpp ${BOTS}/BotCamera.cpp)
else()
    target_sources(object PRIVATE ${BOTS}/ServerExtStub.cpp)
endif()
target_include_directories(object PRIVATE ${BOTS})
set_target_properties(object PROPERTIES PREFIX "" OUTPUT_NAME "object" SUFFIX ".lto")
target_include_directories(object PRIVATE
    ${AVP2}/ServerRes ${AVP2}/ClientShellDLL ${AVP2}/ObjectDLL ${AVP2}/Shared
    ${SDK} ${SDK}/compat ${LS}/incs)
# Object.dsp, "Win32 Final" (the configuration the retail .rez game uses)
target_compile_definitions(object PRIVATE
    _FINAL NDEBUG _OBJECTBUILD _AVP2BUILD LT15_COMPAT)
target_link_libraries(object PRIVATE
    ButeMgrNoMFC GameSpyMgr WONAPI MFCStub CryptMgr StdLith
    wsock32 winmm kernel32 user32 gdi32 advapi32 shell32 ole32 oleaut32 uuid)
# Like retail: image base 0x10000000, no ASLR (addresses in crash reports then
# match object.map / object.pdb; relocations are kept, so the loader can still
# rebase it), no manifest resource. /DEBUG only writes object.pdb next to the
# DLL; the code stays fully optimised (/OPT:REF,ICF restore release linking).
target_compile_options(object PRIVATE /Zi)
target_link_options(object PRIVATE /BASE:0x10000000 /DYNAMICBASE:NO /FIXED:NO /SUBSYSTEM:WINDOWS
    /MANIFEST:NO /INCREMENTAL:NO /DEBUG /PDBALTPATH:%_PDB% /OPT:REF /OPT:ICF /MAP)
