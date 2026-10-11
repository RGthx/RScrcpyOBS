# It may be useful to use a prebuilt server, so that no Android SDK is required
# to build. If the 'prebuilt_server' option is set, just copy the file as is.
prebuilt_server = get_option('prebuilt_server')
if prebuilt_server == ''
    custom_target('scrcpy-server',
                  # gradle is responsible for tracking source changes
                  build_by_default: true,
                  build_always_stale: true,
                  output: 'scrcpy-server',
                  command: [find_program('./scripts/build-wrapper.sh'), meson.current_source_dir(), '@OUTPUT@', get_option('buildtype')],
                  console: true,
                  install: true,
                  install_dir: 'share/scrcpy')
else
    if not prebuilt_server.startswith('/')
        # prebuilt server path is relative to the root scrcpy directory
        prebuilt_server = '../' + prebuilt_server
    endif
    # Windows has no 'cp', and cmd's copy rejects forward-slash paths (which is what
    # meson hands to custom commands). configure_file(copy: true) is meson-native,
    # needs no external program and works with either separator.
    configure_file(input: prebuilt_server,
                   output: 'scrcpy-server',
                   copy: true,
                   install: true,
                   install_dir: 'share/scrcpy')
endif

if meson.version().version_compare('>= 0.58.0')
       devenv = environment()
       devenv.set('SCRCPY_SERVER_PATH', meson.current_build_dir() / 'scrcpy-server')
       meson.add_devenv(devenv)
endif
