{
  description = "CrossPoint Reader development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = inputs: (import ./nix/flake.nix).outputs inputs;
}
