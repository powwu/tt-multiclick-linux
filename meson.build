project(
  'tt-multiclick',
  'c',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

gtk = dependency('gtk+-3.0')
evdev = dependency('libevdev')
x11 = dependency('x11')
xtst = dependency('xtst')
xi = dependency('xi')
ei = dependency('libei-1.0')
thread = dependency('threads')

datadir = get_option('prefix') / get_option('datadir')

add_project_arguments(
  '-DDATADIR="' + datadir + '"',
  language: 'c',
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
