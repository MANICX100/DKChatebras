gtk_dep = dependency('gtk4', version: '>= 4.14')
adw_dep = dependency('libadwaita-1', version: '>= 1.5')
soup_dep = dependency('libsoup-3.0', version: '>= 3.4')
json_dep = dependency('json-glib-1.0', version: '>= 1.8')
secret_dep = dependency('libsecret-1', version: '>= 0.20')

sources = files(
  'main.c',
  'dk-application.c',
  'dk-window.c',
  'dk-chat-store.c',
  'dk-cerebras-client.c',
  'dk-markdown.c',
  'dk-secret.c',
)

resources = gnome.compile_resources(
  'dkchatebras-resources',
  '../data/io.github.dkchatebras.DKChatebras.gresource.xml',
  source_dir: '../data',
  c_name: 'dkchatebras',
)

executable(
  'dkchatebras',
  sources,
  resources,
  include_directories: include_directories('..'),
  dependencies: [gtk_dep, adw_dep, soup_dep, json_dep, secret_dep],
  install: true,
)
