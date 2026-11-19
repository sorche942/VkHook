data_dir = get_option('datadir')

vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

# One manifest serves one install tree. A system install that carries both the
# x86-64 and the i686 build writes the same VkHook.json twice, so the two must be
# byte-identical or the second install clobbers the first. An absolute path
# names one architecture's library, so 'soname' writes the bare file name instead
# and lets each process's dynamic loader find its own architecture's libvkhook.so
# (this needs both libdirs on the loader's search path, as /usr/lib and /usr/lib32
# are on every multilib distro). The Vulkan loader drops a second manifest with the
# same layer name, so per-architecture file names are not an alternative.
if get_option('layer_library_path') == 'soname'
    layer_library_path = 'libvkhook.so'
else
    layer_library_path = join_paths(get_option('prefix'), lib_dir, 'libvkhook.so')
endif

configure_file(
    input : 'VkHook.json.in',
    output : 'VkHook.json',
    configuration : {
        'layer_library_path' : layer_library_path,
        'layer_version' : vkhook_version,
    },
    install : true,
    install_dir : vulkan_layer_dir,
)
