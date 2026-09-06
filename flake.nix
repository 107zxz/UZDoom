{
  inputs = {
    utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, utils }: utils.lib.eachDefaultSystem (system:
    let
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShell = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
            pkg-config
            cmake
            ninja
            gdb
            clang-tools

        ];
        buildInputs = with pkgs; [
            python3
            alsa-lib
            linuxdeploy

            act
        ] ++ pkgs.uzdoom.buildInputs;
      };
    }
  );
}
