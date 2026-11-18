project('VkHook', ['c', 'cpp'], meson_version: '>=1.3.0', default_options: ['c_std=c11', 'cpp_std=c++2a'])

cpp = meson.get_compiler('cpp')

VkHook_include_path = ''

lib_dir = get_option('libdir')

if get_option('append_libdir_vkhook')
    lib_dir = join_paths(lib_dir, 'vkhook')
endif

if get_option('with_so')
    subdir('src')
endif

if get_option('with_json')
    subdir('packaging/meson-config')
endif

# Everything redistributed here is licensed text that has to travel with a
# system install: the fonts (fetched at install time, but shipped by packagers),
# ImGui and reshade.
install_data(
    'assets/font/LICENSE',
    'assets/font/LICENSE-Apache-2.0.txt',
    'assets/font/LICENSE-SIL-OFL-1.1.txt',
    'assets/font/LICENSE-GoogleSansFlex-OFL.txt',
    'src/vendor/reshade/LICENSE.md',
    'src/vendor/imgui/LICENSE.txt',
    install_dir : get_option('datadir') / 'licenses' / 'vkhook',
)
