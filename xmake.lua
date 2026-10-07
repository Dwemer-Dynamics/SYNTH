set_xmakever("3.0.0")
set_project("SYNTH")
set_version("0.1.0")
set_languages("c++23")
set_warnings("allextra")
add_rules("mode.debug", "mode.releasedbg")

option("build_flat")
    set_default(false)
    set_showmenu(true)
    set_description("Build the Windows x64 SYNTH.dll lane")
option_end()

option("commonlibf4_dir")
    set_showmenu(true)
    set_description("Existing pinned libxse/CommonLibF4 checkout")
option_end()

option("menuframework_dir")
    set_showmenu(true)
    set_description("Directory holding the pinned F4SEMenuFramework consumer headers")
option_end()

local commonlibf4_dir = get_config("commonlibf4_dir")
if commonlibf4_dir then
    includes(path.join(commonlibf4_dir, "xmake.lua"))
end

local test_suites = {
    {name = "native", source = "tests/native/native_tests.cpp"},
    {name = "transport", source = "tests/transport/transport_tests.cpp"},
    {name = "json", source = "tests/json/json_tests.cpp"},
    {name = "fake-server", source = "tests/fake_server/fake_server_tests.cpp"},
    {name = "config", source = "tests/config/config_tests.cpp"},
    {name = "diagnostics", source = "tests/diagnostics/diagnostics_tests.cpp"},
    {name = "tasks", source = "tests/tasks/task_lanes_tests.cpp"},
    {name = "lifecycle", source = "tests/lifecycle/lifecycle_tests.cpp"},
    {name = "context", source = "tests/context/context_tests.cpp"},
    {name = "targeting", source = "tests/targeting/targeting_tests.cpp"},
    {name = "actions", source = "tests/actions/actions_tests.cpp"},
    {name = "input", source = "tests/input/input_tests.cpp"},
    {name = "audio", source = "tests/audio/audio_tests.cpp"},
    {name = "presentation", source = "tests/presentation/presentation_tests.cpp"},
    {name = "media-fetch", source = "tests/media_fetch/media_fetch_tests.cpp"},
    {name = "adapters", source = "tests/adapters/adapters_tests.cpp"}
}

local protocol_native_source = "tests/protocol_native/protocol_native_tests.cpp"
if os.isfile(protocol_native_source) then
    table.insert(test_suites, {name = "protocol-native", source = protocol_native_source})
end

for _, suite in ipairs(test_suites) do
    target("synth-" .. suite.name .. "-tests")
        set_kind("binary")
        set_default(true)
        add_files(suite.source)
        add_includedirs("src")
        if is_plat("windows") then
            add_cxxflags("/permissive-", "/EHsc")
        end
        on_run(function(target)
            if suite.name == "protocol-native" then
                os.execv(target:targetfile(), {os.projectdir()})
            else
                os.execv(target:targetfile())
            end
        end)
    target_end()
end

if has_config("build_flat") then
    if not is_plat("windows") or not is_arch("x64") then
        raise("SYNTH.dll requires a Windows x64 toolchain")
    end
    if not os.isfile("src/flat/plugin_entry.cpp") then
        raise("flat lane requested, but src/flat/plugin_entry.cpp does not exist; no DLL will be fabricated")
    end
    if not commonlibf4_dir then
        raise("flat lane requires a pinned CommonLibF4 checkout via --commonlibf4_dir; scripts/build-flat.ps1 fetches the declared pin")
    end
    local menuframework_dir = get_config("menuframework_dir")
    if not menuframework_dir or not os.isfile(path.join(menuframework_dir, "F4SEMenuFramework.h")) then
        raise("flat lane requires the pinned F4SEMenuFramework consumer headers via --menuframework_dir; scripts/build-flat.ps1 fetches the declared pin")
    end

    target("SYNTH")
        set_kind("shared")
        set_filename("SYNTH.dll")
        set_default(true)
        set_arch("x64")
        add_files("src/flat/plugin_entry.cpp")
        add_files("src/adapters/native_fault_recorder.cpp")
        add_headerfiles("src/adapters/**.hpp")
        add_headerfiles("src/ui/**.hpp")
        add_includedirs("src")
        -- Header-only consumer API: no import library, resolved at runtime.
        add_includedirs(menuframework_dir)
        add_deps("commonlibf4")
        add_syslinks("winhttp", "xaudio2", "winmm", "shell32", "windowscodecs", "ole32", "oleaut32", "gdi32")
        add_defines("SYNTH_WITH_F4SE=1", "WIN32_LEAN_AND_MEAN", "NOMINMAX",
                    "_WIN32_WINNT=0x0A00", "WINVER=0x0A00")
    target_end()
end
