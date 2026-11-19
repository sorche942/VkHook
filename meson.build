# VERSION is the single source of the version number: the layer manifest, the
# version baked into the library, the Nix flake and the Flatpak all read it.
project('VkHook', ['c', 'cpp'],
    version : files('VERSION'),
    meson_version : '>=1.3.0',
    default_options : ['c_std=c11', 'cpp_std=c++2a'])

cpp = meson.get_compiler('cpp')

vkhook_version = meson.project_version().strip()

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

# Licensed text that has to travel with a system install. The overlay uses ImGui's
# built-in font, so there are no font licenses to ship.
install_data(
    'src/vendor/reshade/LICENSE.md',
    'src/vendor/imgui/LICENSE.txt',
    install_dir : get_option('datadir') / 'licenses' / 'vkhook',
)
