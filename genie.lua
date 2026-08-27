-- genie, https://github.com/bkaradzic/GENie
-- known working version
-- https://github.com/bkaradzic/bx/blob/51f25ba638b9cb35eb2ac078f842a4bed0746d56/tools/bin/windows/genie.exe

MINGW_ACTION = 'gmake'

if _ACTION == 'clean' then
    os.rmdir('./build')
    os.rmdir('./bin')
    os.rmdir('./obj_vs')
    os.rmdir('./obj_' .. MINGW_ACTION)
end

if _ACTION == MINGW_ACTION then
    -- need a msys2 with clang
    premake.gcc.cc   = 'clang'
    premake.gcc.cxx  = 'clang++'
    premake.gcc.ar   = 'llvm-ar'
    premake.llvm = true
end

-- ---------------------------------------------------------------------------
-- Linux native toolchain (Phase 4.5)
--
-- Selected explicitly with CLUMSY_LINUX=1 rather than by probing the host, so a
-- msys2 gmake build on Windows never accidentally picks up g++-16.
--
-- NOTE: the Makefile at the repository root is the *tested* Linux build path.
-- GENie ships here as a Windows binary, so this block exists to keep one build
-- definition honest rather than because it is exercised on every change. If you
-- add a source file, update both.
-- ---------------------------------------------------------------------------
LINUX_ACTION = 'gmake'
LINUX_CXX = 'g++-16'
LINUX_BUILDOPTIONS = { '--std=c++23' }

function setupLinuxToolchain()
    premake.gcc.cc  = LINUX_CXX
    premake.gcc.cxx = LINUX_CXX
    premake.gcc.ar  = 'gcc-ar-16'
    premake.llvm = false
end

IS_LINUX_TARGET = (os.getenv('CLUMSY_LINUX') == '1')
if IS_LINUX_TARGET and _ACTION == LINUX_ACTION then
    setupLinuxToolchain()
end

local LIB_DIVERT_VC11 = 'external/WinDivert-2.2.0-A'
local LIB_DIVERT_MINGW = 'external/WinDivert-2.2.0-A'

local ROOT = os.getcwd()
print(ROOT)

solution('clumsy')
    location("./build")
    configurations({'Debug', 'Release'})
    platforms({'x64'})

    project('clumsy')
        language("C++")
        -- Phase 4.5: the capture backend, packet helpers, privilege check and
        -- process lookup all exist as a *_win / *_linux pair. Exclude the other
        -- platform's half rather than #ifdef-ing whole files.
        files({'src/**.cpp', 'src/**.h'})
        if IS_LINUX_TARGET then
            excludes({
                'src/divert.cpp', 'src/packetutil_win.cpp',
                'src/elevate.cpp', 'src/procfilter.cpp',
            })
            links({'netfilter_queue', 'nfnetlink', 'mnl', 'pthread', 'dl'})
            buildoptions(LINUX_BUILDOPTIONS)
        else
            excludes({
                'src/divert_linux.cpp', 'src/packetutil_linux.cpp',
                'src/elevate_linux.cpp', 'src/procfilter_linux.cpp',
                'src/platform_linux.cpp',
            })
            links({'WinDivert', 'Winmm', 'ws2_32', 'iphlpapi', 'shell32', 'advapi32'})
        end
        if not IS_LINUX_TARGET then
            if string.match(_ACTION, '^vs') then -- only vs can include rc file in solution
                files({'./etc/clumsy.rc'})
            elseif _ACTION == MINGW_ACTION then
                files({'./etc/clumsy.rc'})
            end
        end

        configuration('Debug')
			flags({'ExtraWarnings', 'Symbols'})
            defines({'_DEBUG'})
            kind("ConsoleApp")

        configuration('Release')
			flags({"Optimize"})
			flags({'Symbols'}) -- keep the debug symbols for development
            defines({'NDEBUG'})
            kind("ConsoleApp") -- Phase 2.3: Release is a console app too.

        configuration(MINGW_ACTION)
            links({'kernel32', 'gdi32', 'comdlg32', 'uuid', 'ole32'}) -- additional libs
            buildoptions({
                '-Wno-missing-braces',
                '-Wno-missing-field-initializers',
                '--std=c++23'
            })
            objdir('obj_'..MINGW_ACTION)

        configuration("vs*")
            defines({"_CRT_SECURE_NO_WARNINGS"})
            flags({'NoManifest'})
            kind("WindowedApp") -- We don't need the console window in VS as we use OutputDebugString().
            buildoptions({'/wd"4214"', '/utf-8', '/std:c++latest'})
			linkoptions({'/ENTRY:"mainCRTStartup" /SAFESEH:NO'})
			-- characterset("MBCS")
            includedirs({LIB_DIVERT_VC11 .. '/include'})
            objdir('obj_vs')

        configuration({'x64', 'vs*'})
            defines({'X64'})
            libdirs({
                LIB_DIVERT_VC11 .. '/x64'
                })

        configuration({'x64', MINGW_ACTION})
            defines({'X64'})
            includedirs({LIB_DIVERT_MINGW .. '/include'})
            libdirs({
                LIB_DIVERT_MINGW .. '/x64'
                })

        local function set_bin(platform, config)
            local platform_str
            if platform == 'vs*' then
                platform_str = 'vs'
            else
                platform_str = platform
            end
            local subdir = ROOT .. '/bin/' .. platform_str .. '/' .. config .. '/x64'
            local divert_lib
            if platform == 'vs*' then
                divert_lib = ROOT .. '/' .. LIB_DIVERT_VC11  .. '/x64/'
            elseif platform == MINGW_ACTION then
                divert_lib = ROOT .. '/' .. LIB_DIVERT_MINGW .. '/x64/'
            end
            configuration({platform, config, 'x64'})
                targetdir(subdir)
                debugdir(subdir)
                if platform == 'vs*' then
                    postbuildcommands({
                        "robocopy " .. divert_lib .." " .. subdir .. '  *.dll *.sys >> robolog.txt',
                        "robocopy " .. ROOT .. "/etc/ "   .. subdir .. ' config.json >> robolog.txt',
                        "robocopy " .. ROOT .. "/etc/ "   .. subdir .. ' config.txt >> robolog.txt',
                        "robocopy " .. ROOT .. "/etc/web/ " .. subdir .. '/web/ *.html >> robolog.txt',
                        "exit /B 0"
                    })
                elseif platform == MINGW_ACTION then
                    postbuildcommands({
                        -- robocopy returns non 0 will fail make
                        'cp ' .. divert_lib .. "WinDivert* " .. subdir,
                        'cp ' .. ROOT .. "/etc/config.json " .. subdir,
                        'cp ' .. ROOT .. "/etc/config.txt " .. subdir,
                        'mkdir -p ' .. subdir .. '/web',
                        'cp ' .. ROOT .. "/etc/web/index.html " .. subdir .. '/web/',
                    })
                end
        end

        set_bin('vs*', 'Debug')
        set_bin('vs*', 'Release')
        set_bin(MINGW_ACTION, 'Debug')
        set_bin(MINGW_ACTION, 'Release')
