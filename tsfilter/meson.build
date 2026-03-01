project('tsfilter', 'c',
  version: '1.0',
  default_options: ['warning_level=3', 'c_std=c11'])

# Find GStreamer dependencies
gst_dep = dependency('gstreamer-1.0', required: true)
gst_base_dep = dependency('gstreamer-base-1.0', required: true)

# Source files for tsfilter plugin
tsfilter_sources = [
  'src/tsfilter.c',
  'src/tsfilterplugin.c',
  'src/tsfilter_crc.c',
]

# Build the plugin as a shared library
tsfilter_library = shared_library('gsttsfilter',
  tsfilter_sources,
  dependencies: [gst_dep, gst_base_dep],
  install: true,
  install_dir: get_option('libdir') / 'gstreamer-1.0'
)

# GStreamer plugin requires a .gstplugin file for discovery
gst_plugin_path = get_option('libdir') / 'gstreamer-1.0'

message('GStreamer include dirs: @0@'.format(gst_dep.get_variable('includedir')))
message('GStreamer plugin install dir: ' + gst_plugin_path)
