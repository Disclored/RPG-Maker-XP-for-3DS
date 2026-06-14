MRuby::Toolchain.new(:gcc) do |conf, params|
  toolchain = params[:toolchain] || 'gcc'

  conf.cc do |cc|
    cc.command = ENV['CC'] || 'gcc'
    cc.flags = [ENV['CFLAGS'] || %w(-O3 -g -Wall -Werror-implicit-function-declaration)]
    cc.compile_options = %(%{flags} -o "%{outfile}" -c "%{infile}")
    cc.preprocess_options = %(%{flags} -E -o "%{outfile}" "%{infile}")
  end

  conf.cxx do |cxx|
    cxx.command = ENV['CXX'] || 'g++'
    cxx.flags = [ENV['CXXFLAGS'] || ENV['CFLAGS'] || %w(-O3 -g -Wall)]
    cxx.compile_options = %(%{flags} -o "%{outfile}" -c "%{infile}")
  end

  conf.archiver do |archiver|
    archiver.command = ENV['AR'] || 'ar'
    archiver.archive_options = 'rs "%{outfile}" %{objs}'
  end

  conf.linker do |linker|
    linker.command = ENV['LD'] || conf.cc.command
    linker.flags = [ENV['LDFLAGS'] || []]
    linker.libraries = %w(m)
    linker.library_paths = []
    linker.link_options = %(%{flags} -o "%{outfile}" %{objs} %{libs})
  end
end
