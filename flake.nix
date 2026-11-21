{
  description = "tsl::hat_trie - header-only C++ trie-based map/set specialized for strings";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # The array hash part of the HAT-trie (the git submodule in third_party/).
    array-hash = {
      url = "github:ivsochenkov/array-hash";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      array-hash,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      # nixpkgs with our overlay applied, so that pkgs.tsl-array-hash and
      # pkgs.tsl-hat-trie are available.
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f (
            import nixpkgs {
              inherit system;
              overlays = [ self.overlays.default ];
            }
          )
        );
    in
    {
      # Use as `nixpkgs.overlays = [ hat-trie.overlays.default ];`
      # to get `pkgs.tsl-hat-trie` (and `pkgs.tsl-array-hash` it depends on).
      overlays.default = nixpkgs.lib.composeManyExtensions [
        array-hash.overlays.default
        (final: _prev: {
          tsl-hat-trie = final.callPackage ./nix/default.nix { };
        })
      ];

      # nix build  /  nix build .#tsl-hat-trie
      packages = forAllSystems (pkgs: {
        inherit (pkgs) tsl-hat-trie;
        default = pkgs.tsl-hat-trie;
      });

      # nix flake check: builds the Boost.Test suite and runs it.
      checks = forAllSystems (pkgs: {
        tests = pkgs.callPackage ./nix/default.nix { withTests = true; };
      });

      # nix develop
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          # Brings in cmake, Boost (with static libs) used by the test suite
          # and tsl-array-hash.
          inputsFrom = [ (pkgs.callPackage ./nix/default.nix { withTests = true; }) ];

          packages = [
            pkgs.ccls
            pkgs.direnv
            pkgs.clang-tools # clangd, clang-format, clang-tidy
            pkgs.doxygen # doxygen.conf
          ]
          ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.gdb ];

          # Typical workflow inside the shell:
          #   cmake -S . -B build -DBUILD_TESTS=ON
          #   cmake --build build && ./build/tests/tsl_hat_trie_tests
        };
      });
    };
}