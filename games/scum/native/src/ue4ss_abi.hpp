// Hand-written declarations of the few UE4SS symbols this mod uses, written to match the public
// RE-UE4SS headers at the pinned commit 83330635 (UE4SS/include/Mod/CppUserModBase.hpp and
// deps/first/LuaMadeSimple/include/LuaMadeSimple/LuaMadeSimple.hpp, MIT). We do not use the full UE4SS
// headers because they pull in Re-UE4SS/UEPseudo, which is Epic-gated.
//
// ABI contract (checked by native/check-imports.py against UE4SS.dll's export table):
//  - every imported member function mangles to an exact UE4SS.dll export (the access specifier is part of
//    the MSVC mangled name, so public/protected below must match the real header);
//  - CppUserModBase's data layout and virtual function order equal the real class, because UE4SS writes
//    the members (constructor) and calls the virtuals through our vtable. Built /MD Release, the same CRT
//    and STL as UE4SS.dll (MSVCP140/VCRUNTIME140), so std::vector/std::wstring/std::string layouts agree.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#define RC_UE4SS_IMPORT __declspec(dllimport)

namespace RC {

namespace LuaMadeSimple {

class Lua {
public:
    using LuaFunction = int (*)(const Lua&);

    // Lua-facing helpers. Every get_* pops the value it reads (lua_remove at force_index), so read the
    // arguments in order from index 1. set_* push a return value; the C function returns how many it pushed.
    RC_UE4SS_IMPORT auto register_function(const std::string& name, const LuaFunction& function) const -> void;
    RC_UE4SS_IMPORT auto get_stack_size() const -> int32_t;
    RC_UE4SS_IMPORT auto discard_value(int32_t force_index = 1) const -> void;
    [[nodiscard]] RC_UE4SS_IMPORT auto is_nil(int32_t force_index = 1) const -> bool;
    RC_UE4SS_IMPORT auto set_nil() const -> void;
    [[nodiscard]] RC_UE4SS_IMPORT auto is_string(int32_t force_index = 1) const -> bool;
    [[nodiscard]] RC_UE4SS_IMPORT auto get_string(int32_t force_index = 1) const -> std::string_view;
    RC_UE4SS_IMPORT auto set_string(std::string_view str) const -> void;
    [[nodiscard]] RC_UE4SS_IMPORT auto is_number(int32_t force_index = 1) const -> bool;
    [[nodiscard]] RC_UE4SS_IMPORT auto get_number(int32_t force_index = 1) const -> double;
    [[nodiscard]] RC_UE4SS_IMPORT auto is_integer(int32_t force_index = 1) const -> bool;
    [[nodiscard]] RC_UE4SS_IMPORT auto get_integer(int32_t force_index = 1) const -> int64_t;
    RC_UE4SS_IMPORT auto set_integer(int64_t number) const -> void;
    RC_UE4SS_IMPORT auto set_bool(bool value) const -> void;

private:
    Lua() = delete;  // only ever received by reference from UE4SS
    Lua(const Lua&) = delete;
};

}  // namespace LuaMadeSimple

using StringType = std::wstring;
using StringViewType = std::wstring_view;

using ModEvent_OnLuaStartSelf = void (*)(class CppUserModBase*, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                                         LuaMadeSimple::Lua&, LuaMadeSimple::Lua*);

// Same member layout and the same virtual functions in the same declaration order as the real class
// (MSVC groups overloads of one name in its vtable, so the order of the overloads matters as well).
// The virtuals are declared but never defined or referenced here: TakaroScumMod overrides every one.
class CppUserModBase {
protected:
    std::vector<std::shared_ptr<void>> GUITabs{};  // real: std::vector<std::shared_ptr<GUI::GUITab>>

public:
    StringType ModName{};
    StringType ModVersion{};
    StringType ModDescription{};
    StringType ModAuthors{};
    StringType ModIntendedSDKVersion{};

public:
    RC_UE4SS_IMPORT CppUserModBase();
    RC_UE4SS_IMPORT virtual ~CppUserModBase();

public:
    virtual auto on_update() -> void = 0;
    virtual auto on_unreal_init() -> void = 0;
    virtual auto on_ui_init() -> void = 0;
    virtual auto on_program_start() -> void = 0;
    virtual auto on_lua_start(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua,
                              LuaMadeSimple::Lua& async_lua, std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void = 0;
    virtual auto on_lua_start(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua,
                              std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void = 0;
    virtual auto on_lua_stop(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua,
                             LuaMadeSimple::Lua& async_lua, std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void = 0;
    virtual auto on_lua_stop(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua,
                             std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void = 0;
    virtual auto on_dll_load(StringViewType dll_name) -> void = 0;
    virtual auto render_tab() -> void = 0;
    virtual auto on_lua_start(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua,
                              LuaMadeSimple::Lua& async_lua, LuaMadeSimple::Lua* hook_lua) -> void = 0;
    virtual auto on_lua_start(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua,
                              LuaMadeSimple::Lua* hook_lua) -> void = 0;
    virtual auto on_lua_stop(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua,
                             LuaMadeSimple::Lua& async_lua, LuaMadeSimple::Lua* hook_lua) -> void = 0;
    virtual auto on_lua_stop(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua,
                             LuaMadeSimple::Lua* hook_lua) -> void = 0;
    virtual auto on_cpp_mods_loaded() -> void = 0;

protected:
    // The ABI-stable event dispatcher (stored outside the object by UE4SS). Call in the constructor.
    RC_UE4SS_IMPORT auto register_on_lua_start(ModEvent_OnLuaStartSelf) -> void;
};

}  // namespace RC
