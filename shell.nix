{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    gtk3
    libevdev
    xorg.libX11
    xorg.libXtst
    xorg.libXi
    libei
    pkg-config
    meson
    ninja
  ];
}
