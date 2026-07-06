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
        ];
        buildInputs = with pkgs; [
            python3
        ] ++ pkgs.uzdoom.buildInputs;
      };
    }
  );
}
