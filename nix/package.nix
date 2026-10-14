{ lib, kernelPackages, buildPackages, src }:
let
  kernel = kernelPackages.kernel;
in
kernel.stdenv.mkDerivation {
  pname = "bcm2835-ws2812";
  version = "unstable";
  inherit src;
  nativeBuildInputs = kernel.moduleBuildDependencies ++ [
    buildPackages.dtc buildPackages.kmod
  ];
  hardeningDisable = [ "pic" "format" ];
  enableParallelBuilding = true;
  buildPhase = ''
    runHook preBuild
    make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
      ${lib.concatStringsSep " " kernelPackages.kernelModuleMakeFlags} \
      M="$PWD" -j"$NIX_BUILD_CORES" modules
    dtc -@ -I dts -O dtb -o bcm2835-ws2812.dtbo bcm2835-ws2812-overlay.dts
    runHook postBuild
  '';
  doCheck = kernel.stdenv.buildPlatform.canExecute kernel.stdenv.hostPlatform;
  nativeCheckInputs = [ buildPackages.python3 ];
  checkPhase = ''
    runHook preCheck
    make check HOSTCC="$CC"
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    # A successful compile alone is insufficient if a different kernel was used.
    test "$(modinfo -F vermagic bcm2835-ws2812.ko | cut -d ' ' -f1)" = ${lib.escapeShellArg kernel.modDirVersion}
    install -Dm644 bcm2835-ws2812.ko "$out/lib/modules/${kernel.modDirVersion}/extra/bcm2835-ws2812.ko"
    install -Dm644 bcm2835-ws2812.dtbo "$out/overlays/bcm2835-ws2812.dtbo"
    install -m644 bcm2835-ws2812-overlay.dts "$out/overlays/bcm2835-ws2812-overlay.dts"
    runHook postInstall
  '';
  passthru = { inherit kernel; kernelVersion = kernel.modDirVersion; };
  meta.platforms = lib.platforms.linux;
}
