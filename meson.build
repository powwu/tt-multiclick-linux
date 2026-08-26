project(
  'tt-multiclick',
  'c',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  language: 'c',
)

gtk    = dependency('gtk+-3.0')
evdev  = dependency('libevdev')
x11    = dependency('x11')
xtst   = dependency('xtst')
xi     = dependency('xi')
ei     = dependency('libei-1.0')
thread = dependency('threads')

conf = configuration_data()
conf.set_quoted(
  'TT_MULTICLICK_DATADIR',
  get_option('prefix') / get_option('datadir') / 'tt-multiclick',
)

configure_file(
  output: 'config.h',
  configuration: conf,
)

executable(
  'tt-multiclick',
  'tt-multiclick.c',
  dependencies: [
    gtk,
    evdev,
    x11,
    xtst,
    xi,
    ei,
    thread,
  ],
  install: true,
)

install_data(
  'tt-multiclick.ui',
  install_dir: get_option('datadir') / 'tt-multiclick',
)
