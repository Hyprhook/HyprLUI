# flake is heavily inspired by
# `https://github.com/KZDKM/Hyprspace`
# `https://github.com/Hyprhook/Hyprhook`
{
  description = "hyprLUI";

  inputs = {
    systems = {
      type = "github";
      owner = "nix-systems";
      repo = "default-linux";
    };
    nixpkgs = {
      type = "github";
      owner = "nixos";
      repo = "nixpkgs";
      ref = "nixos-unstable";
    };
    hyprland = {
      owner = "hyprwm";
      repo = "Hyprland";
      type = "github";
      # ref = "v0.55.1";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
    };
  };

  outputs =
    {
      self,
      systems,
      hyprland,
      ...
    }:
    let
      inherit (builtins)
        concatStringsSep
        elemAt
        head
        readFile
        split
        substring
        ;
      inherit (hyprland.inputs) nixpkgs;

      perSystem =
        attrs:
        nixpkgs.lib.genAttrs (import systems) (
          system:
          attrs system (
            import nixpkgs {
              inherit system;
              overlays = [ hyprland.overlays.hyprland-packages ];
            }
          )
        );

      # Generate version
      mkDate =
        longDate:
        (concatStringsSep "-" [
          (substring 0 4 longDate)
          (substring 4 2 longDate)
          (substring 6 2 longDate)
        ]);

      version =
        (head (split "'" (elemAt (split " version: '" (readFile ./meson.build)) 2)))
        + "+date=${mkDate (self.lastModifiedDate or "19700101")}_${self.shortRev or "dirty"}";
    in
    {
      packages = perSystem (
        system: pkgs: {
          HyprLUI =
            let
              hyprlandPkg = hyprland.packages.${system}.hyprland;
            in
            pkgs.gcc14Stdenv.mkDerivation {
              pname = "HyprLUI";
              inherit version;
              src = ./.;

              inherit (hyprlandPkg) nativeBuildInputs;
              buildInputs = [ hyprlandPkg ] ++ hyprlandPkg.buildInputs;
              dontUseCmakeConfigure = true;

              installFlags = [ "PREFIX=$(out)" ];

              postInstall = ''
                mv $out/lib/HyprLUI.so $out/lib/libHyprLUI.so
                install -D -m 0644 stubs/hyprlui.meta.lua $out/share/hypr/stubs/hyprlui.meta.lua
                mkdir -p $out/share/hypr/hyprlui/demos
                cp -r demos/* $out/share/hypr/hyprlui/demos/
              '';

              meta = with pkgs.lib; {
                homepage = "https://github.com/Hyprhook/HyprLUI";
                description = "";
                license = licenses.mit;
                platforms = platforms.linux;
              };
            };
          default = self.packages.${system}.HyprLUI;

          # demos/notification-manager/daemon - see that file's own header
          # comment for why this is a standalone Lua 5.1 executable (ldbus
          # has no Lua 5.5 build upstream, so it can't run inside
          # Hyprland/HyprLUI's own embedded interpreter). Wraps the plain
          # `.lua` script with LUA_CPATH/LUA_PATH baked in via
          # makeWrapper, so it runs standalone with no `nix develop`
          # indirection - what services.hyprlui-notification-daemon
          # (homeManagerModules below) actually execs.
          notification-daemon = pkgs.stdenvNoCC.mkDerivation {
            pname = "hyprlui-notification-daemon";
            inherit version;
            src = ./demos/notification-manager/daemon;
            dontBuild = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];

            installPhase = ''
              install -D -m 0755 notification-daemon.lua $out/share/hyprlui-notification-daemon/notification-daemon.lua
              makeWrapper ${pkgs.lua5_1}/bin/lua $out/bin/hyprlui-notification-daemon \
                --set LUA_CPATH "${pkgs.lua51Packages.ldbus}/lib/lua/5.1/?.so;${pkgs.lua51Packages.luasocket}/lib/lua/5.1/?.so" \
                --set LUA_PATH "${pkgs.lua51Packages.luasocket}/share/lua/5.1/?.lua;;" \
                --add-flags "$out/share/hyprlui-notification-daemon/notification-daemon.lua"
            '';

            meta = with pkgs.lib; {
              description = "Standalone org.freedesktop.Notifications D-Bus daemon for HyprLUI's notification-manager demo";
              license = licenses.mit;
              platforms = platforms.linux;
              mainProgram = "hyprlui-notification-daemon";
            };
          };
        }
      );

      devShells = perSystem (
        system: pkgs: {
          default = pkgs.mkShell {
            name = "HyprLUI-shell";
            nativeBuildInputs = with pkgs; [
              gcc14
              clang-tools
              bear
            ];
            buildInputs = [ hyprland.packages.${system}.hyprland ];
            inputsFrom = [
              hyprland.packages.${system}.hyprland
              self.packages.${system}.HyprLUI
            ];
            shellHook = ''
              meson setup build --reconfigure
              sed -e 's/c++23/c++2b/g' ./build/compile_commands.json > ./compile_commands.json
              export HYPRLAND_LUA_STUBS="${hyprland.packages.${system}.hyprland}/share/hypr/stubs"
            '';
          };

          # demos/notification-manager/daemon - a standalone Lua 5.1
          # process (NOT the Lua 5.5 Hyprland/HyprLUI itself embeds -
          # ldbus has no Lua 5.5 build upstream). ldbus talks real D-Bus
          # (org.freedesktop.Notifications); luasocket hosts the Unix
          # socket HyprLUI's own open_socket() connects to as a client.
          # Deliberately separate from `default` above - that shell's own
          # hook runs a full C++ `meson setup --reconfigure` on every
          # entry, which the systemd unit starting this daemon shouldn't
          # pay for just to get a Lua interpreter on its PATH.
          notification-daemon = pkgs.mkShell {
            name = "HyprLUI-notification-daemon-shell";
            nativeBuildInputs = with pkgs; [
              lua5_1
              lua51Packages.ldbus
              lua51Packages.luasocket
            ];
            shellHook = ''
              export LUA_CPATH="${pkgs.lua51Packages.ldbus}/lib/lua/5.1/?.so;${pkgs.lua51Packages.luasocket}/lib/lua/5.1/?.so"
              export LUA_PATH="${pkgs.lua51Packages.luasocket}/share/lua/5.1/?.lua;;"
            '';
          };
        }
      );

      formatter = perSystem (_: pkgs: pkgs.alejandra);

      # `services.hyprlui-notification-daemon.enable = true;` in a
      # home-manager config is the toggle - `systemd.user.services.*` is
      # what mako.service itself (per its own installed unit file) was
      # already managed through, so this matches that same pattern rather
      # than introducing a new one. `enable = false` (the default) means
      # the service simply isn't defined - no leftover masked/disabled
      # unit to clean up, unlike a manually `systemctl --user mask`'d one.
      homeManagerModules.notification-daemon =
        {
          config,
          lib,
          pkgs,
          ...
        }:
        let
          cfg = config.services.hyprlui-notification-daemon;
        in
        {
          options.services.hyprlui-notification-daemon.enable = lib.mkEnableOption "the HyprLUI notification-manager daemon (org.freedesktop.Notifications - see demos/notification-manager)";

          config = lib.mkIf cfg.enable {
            systemd.user.services.notification-daemon = {
              Unit = {
                Description = "HyprLUI notification daemon (org.freedesktop.Notifications)";
                PartOf = [ "graphical-session.target" ];
                After = [ "graphical-session.target" ];
              };
              Service = {
                # Type=dbus + BusName, matching mako.service's own
                # convention - systemd waits for the name to actually be
                # claimed before considering this unit started.
                Type = "dbus";
                BusName = "org.freedesktop.Notifications";
                ExecStart = "${self.packages.${pkgs.system}.notification-daemon}/bin/hyprlui-notification-daemon";
                Restart = "on-failure";
              };
              Install.WantedBy = [ "graphical-session.target" ];
            };
          };
        };
      homeManagerModules.default = self.homeManagerModules.notification-daemon;
    };
}
