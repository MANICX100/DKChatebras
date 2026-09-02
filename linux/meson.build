project(
  'dkchatebras',
  'c',
  version: '1.0.0',
  meson_version: '>= 1.3.0',
  default_options: ['c_std=c17', 'warning_level=2'],
)

gnome = import('gnome')
i18n = import('i18n')

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
datadir = prefix / get_option('datadir')

conf = configuration_data()
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('APP_ID', 'io.github.dkchatebras.DKChatebras')
configure_file(output: 'config.h', configuration: conf)

subdir('src')
subdir('data')

summary({
  'Version': meson.project_version(),
  'Prefix': prefix,
  'Build type': get_option('buildtype'),
}, section: 'DKChatebras')
