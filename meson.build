project(
  'autorun', 'cpp',
  version : '0.1',
  license : 'MIT',
  default_options: ['cpp_std=c++17', 'warning_level=3', 'werror=true']
)

config = configuration_data()
config.set('version', meson.project_version())
config.set('debug', 'true')
configure_file(
  input: 'config.h.in',
  output: 'config.h',
  configuration: config
)

executable('autorun', 'autorun.cpp', install : true)
