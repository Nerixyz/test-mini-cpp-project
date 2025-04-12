{
  description = "repro";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { nixpkgs, flake-utils, ...  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
    in {
      devShells.default = pkgs.stdenvNoCC.mkDerivation {
        name = "repro-shell";
        nativeBuildInputs = with pkgs; [ openssl.dev boost186 go ninja cmake gcc14 ];
        env = { CMAKE_GENERATOR = "Ninja"; };
      };
    });
}
