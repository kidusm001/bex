{
  description = "bex - Native Linux performance monitor built with Qt 6";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs:
        let
          bex = pkgs.stdenv.mkDerivation {
            pname = "bex";
            version = "1.0.0";
            src = self;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsHook
            ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtwayland
              pkgs.xorg.libxcb
            ];

            cmakeFlags = [
              "-DBEX_BUILD_TESTS=OFF"
              "-DBEX_X11=ON"
            ];

            meta = with pkgs.lib; {
              description = "Native Linux performance monitor built with Qt 6";
              homepage = "https://github.com/kidusm001/bex";
              license = licenses.mit;
              platforms = platforms.linux;
              mainProgram = "bex";
            };
          };
        in
        {
          inherit bex;
          default = bex;
        });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.default}/bin/bex";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          packages = [ pkgs.clang-tools pkgs.gdb ];
        };
      });
    };
}
