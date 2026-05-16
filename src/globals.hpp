#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <string>

namespace Global {
    inline HANDLE     PHANDLE         = nullptr;
    const std::string pluginName      = "HyprLUI";
    const std::string pluginNameShort = "HLUI";
    const std::string version         = "0.1";
    const std::string configPName     = "hyprLUI";
    const std::string author          = "Moritz Gleissner";
    const std::string description     = "";
    inline bool       hyprlandReady   = false;
} // namespace global
