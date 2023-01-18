{ lib
, stdenv
, rustPlatform
, llvmPackages
, bcachefs
, pkg-config
, udev
, liburcu
, zstd
, libaio
, lz4       # liblz4
, libsodium
, libuuid
, zlib       # zlib1g
, libscrypt
, rustfmt
, glibc
, ...
}:
let
  cargo = lib.trivial.importTOML ./Cargo.toml;
in
rustPlatform.buildRustPackage {
  pname = cargo.package.name;
  version = cargo.package.version;

  src = builtins.path {
    path = ./.;
    name = "bch_bindgen";
  };

  cargoLock = { lockFile = ./Cargo.lock; };

  propagatedNativeBuildInputs = [ rustPlatform.bindgenHook ];

  propagatedBuildInputs = [
    bcachefs.tools
  ];

  LIBBCACHEFS_LIB ="${bcachefs.tools}/lib";
  LIBBCACHEFS_INCLUDE = bcachefs.tools.src;

  postPatch = ''
    cp ${./Cargo.lock} Cargo.lock
  '';

  doCheck = true;
}
