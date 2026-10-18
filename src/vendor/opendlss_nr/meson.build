# OpenDLSS-NR host code and unchanged GPU kernels. No demo, volk globals, or CUDA SDK.
opendlss_nr_python = import('python').find_installation('python3')
opendlss_nr_glslang = find_program('glslang', 'glslangValidator')
opendlss_nr_include = include_directories('.')
opendlss_nr_build_shader_dir = meson.current_build_dir()
opendlss_nr_build_ptx_dir = meson.current_build_dir()
opendlss_nr_data_dir = get_option('prefix') / get_option('datadir') / 'vkintox' / 'dlss5'
opendlss_nr_install_shader_dir = opendlss_nr_data_dir / 'shaders'
opendlss_nr_install_ptx_dir = opendlss_nr_data_dir / 'ptx'
opendlss_nr_assets = []

opendlss_nr_shader_includes = files('shaders/common.glsl', 'shaders/attention_common.glsl', 'shaders/publish.glsl')
foreach shader : ['fused_block32', 'gemm_f16', 'gemm_fp8', 'gemm_mlp', 'gemm_reduce',
                  'global_attend', 'global_attention', 'global_normalize', 'ops', 'preprocess',
                  'qkv_attention', 'window_attend', 'window_normalize']
    opendlss_nr_assets += custom_target(
        'opendlss_nr_' + shader,
        input : 'shaders' / (shader + '.comp'),
        output : shader + '.spv',
        command : [opendlss_nr_glslang, '-V', '--target-env', 'vulkan1.3',
                   '-I' + (meson.current_source_dir() / 'shaders'), '@INPUT@', '-o', '@OUTPUT@'],
        depend_files : opendlss_nr_shader_includes,
        build_by_default : true,
        install : true,
        install_dir : opendlss_nr_install_shader_dir,
    )
endforeach

foreach shader : ['dlss5_preprocess', 'dlss5_composite', 'dlss5_reproject', 'dlss5_luma']
    opendlss_nr_assets += custom_target(shader,
        input : '../../effects/shaders/glsl' / (shader + '.comp'),
        output : shader + '.spv',
        command : [opendlss_nr_glslang, '-V', '--target-env', 'vulkan1.3',
            '-I' + (meson.current_source_dir() / 'shaders'), '@INPUT@', '-o', '@OUTPUT@'],
        depend_files : opendlss_nr_shader_includes + files(
            '../../effects/shaders/glsl/dlss5_common.glsl', '../../effects/shaders/glsl/dlss5_debug.glsl'),
        build_by_default : true, install : true, install_dir : opendlss_nr_install_shader_dir)
endforeach

opendlss_nr_ptx_sources = files(
    'ptx/block32_e4m3.py', 'ptx/ffn_e4m3.py', 'ptx/gemm2_e4m3.py', 'ptx/gemmt_e4m3.py',
    'ptx/gemmv_e4m3.py', 'ptx/global_attention_e4m3.py', 'ptx/global_attention_stream_e4m3.py',
    'ptx/mlp_e4m3.py', 'ptx/ptxgen.py', 'ptx/qkv_e4m3.py', 'ptx/reduce_e4m3.py', 'ptx/swin.py',
)
opendlss_nr_ptx_builder = files('build_ptx.py')
opendlss_nr_ptx_names = run_command(opendlss_nr_python, opendlss_nr_ptx_builder, '--list', check : true).stdout().strip().split('\n')
foreach name : opendlss_nr_ptx_names
    opendlss_nr_assets += custom_target(
        'opendlss_nr_' + name,
        output : name,
        command : [opendlss_nr_python, opendlss_nr_ptx_builder, '--generate', name, '--output', '@OUTPUT@'],
        depend_files : opendlss_nr_ptx_sources + opendlss_nr_ptx_builder,
        build_by_default : true,
        install : true,
        install_dir : opendlss_nr_install_ptx_dir,
    )
endforeach

opendlss_nr_library = static_library(
    'opendlss_nr',
    files('vk_context.cpp', 'nr_model.cpp', 'kernels.cpp', 'nr_graph.cpp', 'reference.cpp'),
    include_directories : opendlss_nr_include,
    cpp_args : ['-DVK_ENABLE_BETA_EXTENSIONS', '-DVK_NO_PROTOTYPES', '-fno-fast-math', '-ffp-contract=off'],
    override_options : ['cpp_std=c++20'],
    gnu_symbol_visibility : 'hidden',
    pic : true,
    install : false,
)
opendlss_nr_dep = declare_dependency(
    link_with : opendlss_nr_library,
    include_directories : opendlss_nr_include,
    compile_args : ['-DVK_ENABLE_BETA_EXTENSIONS'],
)
install_data('LICENSE', 'NOTICE', install_dir : get_option('datadir') / 'licenses' / 'vkintox' / 'opendlss_nr')
