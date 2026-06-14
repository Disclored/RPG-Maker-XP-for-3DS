# Cross Compiling configuration for Nintendo 3DS (devkitARM)
# 3DS CPU: ARM11 MPCore (armv6k), hard-float VFPv2
MRuby::CrossBuild.new("nintendo_3ds") do |conf|
  DEVKITPRO_PATH = ENV.fetch("DEVKITPRO", "/opt/devkitpro")
  BIN_PATH       = "#{DEVKITPRO_PATH}/devkitARM/bin"
  LIBCTRU        = "#{DEVKITPRO_PATH}/libctru"

  ARM_FLAGS = %w[
    -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
    -O2 -ffunction-sections -fdata-sections
    -D__3DS__ -DARM_32BIT
    -std=gnu17
    -Wno-error=incompatible-pointer-types
    -Wno-error=implicit-function-declaration
    -Wno-error=int-conversion
    -Wno-error=implicit-int
  ]

  conf.cc do |cc|
    cc.command = "#{BIN_PATH}/arm-none-eabi-gcc"
    cc.flags   << ARM_FLAGS
    cc.include_paths << "#{LIBCTRU}/include"
    cc.compile_options = %(%{flags} -o "%{outfile}" -c "%{infile}")
  end

  conf.cxx do |cxx|
    cxx.command         = "#{BIN_PATH}/arm-none-eabi-g++"
    cxx.include_paths   = conf.cc.include_paths.dup
    cxx.flags           = conf.cc.flags.dup
    cxx.flags           << %w[-fno-rtti -fno-exceptions -std=c++11]
    cxx.defines         = conf.cc.defines.dup
    cxx.compile_options = conf.cc.compile_options.dup
  end

  conf.archiver do |ar|
    ar.command = "#{BIN_PATH}/arm-none-eabi-ar"
  end

  conf.linker do |linker|
    linker.command = "#{BIN_PATH}/arm-none-eabi-gcc"
    linker.flags   << ARM_FLAGS
    linker.libraries << "m"
  end

  conf.bins = []
  conf.build_mrbtest_lib_only
  conf.disable_cxx_exception

  # Gems seguros para embedded (sem socket, sem process, sem sleep)
  conf.gem core: "mruby-sprintf"
  conf.gem core: "mruby-print"
  conf.gem core: "mruby-math"
  conf.gem core: "mruby-struct"
  conf.gem core: "mruby-compar-ext"
  conf.gem core: "mruby-enum-ext"
  conf.gem core: "mruby-string-ext"
  conf.gem core: "mruby-numeric-ext"
  conf.gem core: "mruby-array-ext"
  conf.gem core: "mruby-hash-ext"
  conf.gem core: "mruby-range-ext"
  conf.gem core: "mruby-proc-ext"
  conf.gem core: "mruby-symbol-ext"
  conf.gem core: "mruby-random"
  conf.gem core: "mruby-object-ext"
  conf.gem core: "mruby-objectspace"
  conf.gem core: "mruby-fiber"
  conf.gem core: "mruby-enumerator"
  conf.gem core: "mruby-enum-lazy"
  conf.gem core: "mruby-toplevel-ext"
  conf.gem core: "mruby-kernel-ext"
  conf.gem core: "mruby-class-ext"
  conf.gem core: "mruby-compiler"
  conf.gem core: "mruby-eval"
  conf.gem core: "mruby-metaprog"
  conf.gem core: "mruby-pack"
  conf.host_target = 'arm-none-eabi'
  conf.gem File.expand_path("~/mruby-onig-regexp")
end