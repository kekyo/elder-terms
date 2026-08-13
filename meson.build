project(
  'elder-terms',
  'cpp',
  version: '0.0.1',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true',
  ],
)

elder_terms_private_install_dir = get_option('libdir') / 'elder-terms'
elder_terms_private_install_rpath = '$ORIGIN/..'

subdir('shared')
subdir('elder-terms')
subdir('elder-terms-vte')
subdir('po')

install_data(
  [
    'README.md',
    'README_ja.md',
    'packaging/copyright',
  ],
  install_dir: get_option('datadir') / 'doc/elder-terms',
)
