{
  lib,
  stdenv,
  cmake,
  boost,
  tsl-array-hash,
  # Build and run the Boost.Test suite (tests/). Off by default so that a plain
  # `nix build` is fast and does not need Boost; `nix flake check` enables it.
  withTests ? false,
}:

let
  # Keep the package version in sync with `project(tsl_hat_trie VERSION x.y.z)`.
  version =
    let
      matches = map (builtins.match "project\\(tsl_hat_trie VERSION ([0-9.]+)\\).*") (
        lib.splitString "\n" (builtins.readFile ../CMakeLists.txt)
      );
    in
    builtins.head (builtins.head (lib.filter (m: m != null) matches));

  # tests/CMakeLists.txt sets Boost_USE_STATIC_LIBS, and nixpkgs' boost only
  # ships shared libraries by default.
  boostWithStatic = boost.override { enableStatic = true; };
in
stdenv.mkDerivation {
  pname = "tsl-hat-trie";
  inherit version;

  # third_party/ (the array-hash git submodule) is intentionally left out:
  # array-hash is provided by the `tsl-array-hash` package, which CMakeLists.txt
  # picks up through find_package(tsl-array-hash) when the submodule is absent.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions (
      [
        ../CMakeLists.txt
        ../cmake
        ../include
      ]
      ++ lib.optional withTests ../tests
    );
  };

  strictDeps = true;

  nativeBuildInputs = [ cmake ];

  # Header-only library: consumers of tsl-hat-trie also need the array-hash
  # headers (included from <tsl/htrie_hash.h>), and the installed CMake config
  # calls find_dependency(tsl-array-hash).
  propagatedBuildInputs = [ tsl-array-hash ];

  buildInputs = lib.optionals withTests [ boostWithStatic ];

  cmakeFlags = [
    (lib.cmakeBool "BUILD_TESTS" withTests)
  ];

  doCheck = withTests;

  checkPhase = ''
    runHook preCheck
    ./tests/tsl_hat_trie_tests
    runHook postCheck
  '';

  meta = {
    description = "C++ implementation of a fast and memory efficient HAT-trie (trie-based map and set for strings)";
    homepage = "https://github.com/ivsochenkov/hat-trie";
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
  };
}