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
              '';

              meta = with pkgs.lib; {
                homepage = "https://github.com/Hyprhook/HyprLUI";
                description = "";
                license = licenses.mit;
                platforms = platforms.linux;
              };
            };
          default = self.packages.${system}.HyprLUI;
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
        }
      );

      formatter = perSystem (_: pkgs: pkgs.alejandra);
    };
}
