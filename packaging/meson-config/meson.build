data_dir = get_option('datadir')

vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

configure_file(
    input : 'VkHook.json.in',
    output : 'VkHook.json',
    configuration : {'layer_library_path' : join_paths(get_option('prefix'), lib_dir, 'libvkhook.so')},
    install : true,
    install_dir : vulkan_layer_dir,
)
