markdown_test = executable(
  'test-markdown',
  'test-markdown.c',
  '../src/dk-markdown.c',
  include_directories: include_directories('../src'),
  dependencies: gtk_dep,
)

test('markdown rendering', markdown_test)
