project('HyprLUI', 'cpp',
  version: '0.1',
  # 'debugoptimized' (-O2 -g) instead of 'release' (-O3, no debug info) -
  # a crash report is useless without this: without -g, a backtrace is
  # just raw offsets into whichever exported symbol happens to be
  # nearest (an unexported lambda gets folded into its enclosing
  # function's own symbol with no way to tell them apart), not an actual
  # file:line. Cost is a bit of binary size and -O2 vs -O3, not worth
  # trading away the ability to actually root-cause a crash.
  default_options: ['buildtype=debugoptimized'],
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('Could not configure current C++ compiler (' + cpp_compiler.get_id() + ' ' + cpp_compiler.version() + ') with required C++ standard (C++23)')
endif

add_project_arguments([ '-DWLR_USE_UNSTABLE' ], language: 'cpp')

globber = run_command('find', '.', '-name', '*.cpp', check: true)
src = globber.stdout().strip().split('\n')

shared_module(meson.project_name(), src,
  dependencies: [
    dependency('hyprland'),
    dependency('pixman-1'),
    dependency('libdrm'),
    dependency('lua5.5'),
    dependency('hyprgraphics'),
    # dependency('pangocairo'),
    # dependency('libinput'),
    # dependency('libudev'),
    # dependency('wayland-server'),
    # dependency('xkbcommon')
  ],
  install: true,
)
