{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { nixpkgs, ... }: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
  in {
    devShells.${system}.default = pkgs.mkShell {
      buildInputs = with pkgs; [
        cmake
        llvmPackages.libclang
        llvmPackages.clang
        protobuf
        pkg-config
      ];
      shellHook = ''
        export LIBCLANG_PATH="${pkgs.llvmPackages.libclang.lib}/lib"
      '';
    };
  };
}
