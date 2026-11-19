subdir('vendor/reshade')

glsl_shader_src = files(
    'effects/shaders/glsl/depth_resolve.frag.glsl',
    'effects/shaders/glsl/depth_resolve_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal_msaa.frag.glsl',
    'effects/shaders/glsl/full_screen_triangle.vert.glsl',
)
glsl_compiler = find_program('glslangValidator')
glsl_generator = generator(glsl_compiler,
    output: ['@BASENAME@.h'],
    arguments: ['-V', '-x', '@INPUT@', '-o', '@OUTPUT@'])
shader_include = glsl_generator.process(glsl_shader_src)

dlss5_assets = []
dlss5_sources = []
dlss5_dependencies = []
dlss5_args = []
dlss5_fs = import('fs')
dlss5_requested = get_option('dlss5')
dlss5_available = false
if not dlss5_requested.disabled()
    dlss5_headers = cpp.has_type('VkPhysicalDeviceShaderFloat8FeaturesEXT',
        prefix : '#include <vulkan/vulkan.h>')
    dlss5_compiler_version = run_command(glsl_compiler, '--version', check : true).stdout()
    dlss5_glslang = dlss5_compiler_version.split('\n')[0].split(':')[-1].strip().version_compare('>=16.6.0')
    dlss5_available = dlss5_headers and dlss5_glslang and dlss5_fs.exists(get_option('dlss5_vulkan_registry'))
    if dlss5_requested.enabled() and not dlss5_available
        error('DLSS5 needs float8 Vulkan headers, their vk.xml registry, and glslang >= 16.6')
    endif
endif
if dlss5_available
    subdir('vendor/opendlss_nr')
    dlss5_paths = configuration_data()
    dlss5_paths.set_quoted('DLSS5_SHADER_DIR', opendlss_nr_install_shader_dir)
    dlss5_paths.set_quoted('DLSS5_PTX_DIR', opendlss_nr_install_ptx_dir)
    dlss5_library_dir = get_option('prefix') / lib_dir
    dlss5_paths.set_quoted('DLSS5_SHADER_RELATIVE_DIR',
        dlss5_fs.relative_to(opendlss_nr_install_shader_dir, dlss5_library_dir))
    dlss5_paths.set_quoted('DLSS5_PTX_RELATIVE_DIR',
        dlss5_fs.relative_to(opendlss_nr_install_ptx_dir, dlss5_library_dir))
    configure_file(output : 'dlss5_paths.hh', configuration : dlss5_paths)
    dlss5_chain = custom_target('dlss5 device feature chain',
        input : get_option('dlss5_vulkan_registry'),
        output : 'dlss5_device_chain.inc',
        command : [opendlss_nr_python, files('../scripts/generate-dlss5-device-chain.py'), '@INPUT@', '@OUTPUT@'])
    dlss5_sources += [files('effects/effect_dlss5.cc', 'render/dlss5_device.cc', 'render/dlss5_optical_flow.cc'), dlss5_chain]
    dlss5_dependencies += opendlss_nr_dep
    dlss5_args += '-DVKHOOK_DLSS5=1'
    dlss5_assets += opendlss_nr_assets
endif
summary('DLSS5 PTX backend', dlss5_available)

imgui_src = [
    'vendor/imgui/imgui.cc',
    'vendor/imgui/imgui_draw.cc',
    'vendor/imgui/imgui_tables.cc',
    'vendor/imgui/imgui_widgets.cc',
    'vendor/imgui/backends/imgui_impl_vulkan.cc',
]

overlay_src = [
    'overlay/imgui_overlay.cc',
    'overlay/view_add_effects.cc',
    'overlay/view_advanced.cc',
    'overlay/view_configs.cc',
    'overlay/view_diagnostics.cc',
    'overlay/view_about.cc',
    'overlay/view_settings.cc',
    'overlay/view_shader_manager.cc',
    'overlay/view_shader_test.cc',
    'overlay/view_main.cc',
    'overlay/params/field_editor.cc',
]

VkHook_src = [
    'layer/entry.cc',
    'layer/state.cc',
    'layer/instance.cc',
    'layer/device.cc',
    'present/swapchain.cc',
    'present/present.cc',
    'present/submit.cc',
    'depth/hooks.cc',
    'depth/tracker.cc',
    'control/frontend.cc',
    'control/preset_import.cc',
    'diag/gpu_crash.cc',
    'core/config/persist.cc',
    'core/config/ini.cc',
    'render/buffer.cc',
    'render/command_buffer.cc',
    'render/vk_handle.cc',
    'core/config/config.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/settings_manager.cc',
    'render/descriptor_set.cc',
    'effects/effect_registry.cc',
    'effects/effect_reshade.cc',
    'effects/reshade_texture_pool.cc',
    'effects/effect_transfer.cc',
    'effects/effect_renderpass.cc',
    'effects/texture_upload.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    'effects/shader_cache.cc',
    'effects/reshade_module_io.cc',
    'effects/texture_convert.cc',
    'render/fake_swapchain.cc',
    'render/format.cc',
    'render/framebuffer.cc',
    'render/graphics_pipeline.cc',
    'render/image.cc',
    'render/image_view.cc',
    'input/keyboard_input.cc',
    'core/logger.cc',
    'render/logical_swapchain.cc',
    'render/memory.cc',
    'render/renderpass.cc',
    'effects/reshade_uniforms.cc',
    'render/sampler.cc',
    'render/shader.cc',
    'vendor/stb/stb_image.c',
    'vendor/stb/stb_image_resize.c',
    'core/util.cc',
    'render/vkdispatch.cc',
]

x11_dep = dependency('x11')
xi_dep = dependency('xi')
# XTest hands held keys and buttons to the game across a grab transition.
xtst_dep = dependency('xtst')
wayland_client_dep = dependency('wayland-client')
xkbcommon_dep = dependency('xkbcommon')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_scanner = find_program('wayland-scanner')

# wp_viewporter scales the 1x1 input-capture buffer to the hitbox
wl_protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')
viewporter_xml = join_paths(wl_protocols_dir, 'stable', 'viewporter', 'viewporter.xml')

viewporter_client_h = custom_target('viewporter-client-header',
    input: viewporter_xml,
    output: 'viewporter-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

viewporter_code = custom_target('viewporter-code',
    input: viewporter_xml,
    output: 'viewporter-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

conf_paths = configuration_data()
conf_paths.set_quoted('SYSCONFDIR', join_paths(get_option('prefix'), get_option('sysconfdir')))
conf_paths.set_quoted('DATADIR', join_paths(get_option('prefix'), get_option('datadir')))

configure_file(output: 'config_paths.hh',
               configuration : conf_paths)

# the binary reports its own version, so it must not depend on its filename or
# on the setup script dropping a version file next to the config.
fs = import('fs')
version_string = 'unknown'
if fs.exists('../VERSION')
    version_string = fs.read('../VERSION').strip()
    if version_string == ''
        version_string = 'unknown'
    endif
endif

conf_version = configuration_data()
conf_version.set_quoted('VKHOOK_VERSION', version_string)

configure_file(output: 'version.hh',
               configuration : conf_version)

# X11/X.h library contains many definitions (e.g. None)
# which conflicts with the rest codebase with with Meson --unity=on option
keyboard_input_x11_lib = static_library('keyboard_input_x11', 'input/keyboard_input_x11.cc', include_directories: [include_directories('core', 'core/config')])
mouse_input_lib = static_library('mouse_input', 'input/mouse_input.cc', include_directories: [include_directories('core', 'core/config')])
input_blocker_lib = static_library('input_blocker', 'input/input_blocker.cc', include_directories: [include_directories('core', 'core/config')])

# X11 input backends. The shared connection (x11_display) owns the single event
# queue; keyboard and mouse register callbacks into it and the blocker grabs on
# the same connection. Kept out of the unity-compiled core because X11/X.h
# defines symbols (None, ...) that collide with the codebase.
x11_display_lib = static_library('x11_display',
    'input/x11_display.cc',
    include_directories: [include_directories('core', 'core/config')])
mouse_input_x11_lib = static_library('mouse_input_x11',
    'input/mouse_input_x11.cc',
    include_directories: [include_directories('core', 'core/config')])

# Wayland input backends — built as static libs to isolate wayland-client headers.
# wayland_input_common provides a shared seat/queue used by both keyboard and mouse.
wayland_input_common_lib = static_library('wayland_input_common',
    'input/wayland_input_common.cc', 'input/wayland_display.cc',
    viewporter_code, viewporter_client_h,
    dependencies: [wayland_client_dep],
    include_directories: [include_directories('core', 'core/config')])
keyboard_input_wayland_lib = static_library('keyboard_input_wayland',
    'input/keyboard_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    include_directories: [include_directories('core', 'core/config')])
mouse_input_wayland_lib = static_library('mouse_input_wayland',
    'input/mouse_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
    include_directories: [include_directories('core', 'core/config')])
# Explicit-sync commit gate for the wayland interpose: keeps a foreign window
# commit out of an open linux-drm-syncobj point sequence.
wayland_syncobj_gate_lib = static_library('wayland_syncobj_gate',
    'input/wayland_syncobj_gate.cc',
    include_directories: [include_directories('core', 'core/config')])
# Wayland event interposition — wraps game's pointer/keyboard listeners to
# suppress events when overlay has input blocked. Needs default visibility
# for the wl_proxy_add_listener symbol override.
wayland_interpose_lib = static_library('wayland_interpose',
    'input/wayland_interpose.cc', 'input/wayland_interpose_patch.cc',
    link_with: [wayland_syncobj_gate_lib],
    dependencies: [wayland_client_dep],
    c_args: ['-fvisibility=default'],
    cpp_args: ['-fvisibility=default'],
    include_directories: [include_directories('core', 'core/config')])

imgui_inc = include_directories('vendor/imgui')
overlay_inc = include_directories('overlay')
effects_inc = include_directories('effects')
effects_params_inc = include_directories('effects/params')
core_inc = include_directories('core', 'core/config')
input_inc = include_directories('input')
render_inc = include_directories('render')
vendor_inc = include_directories('vendor')
source_inc = include_directories('.', 'core', 'core/config', 'input', 'render', 'effects', 'overlay')
stb_inc = include_directories('vendor/stb')
shader_asset_inc = include_directories('effects/shaders')

# The XDG desktop portal is reached over the session bus through GIO, which is
# present on the desktop and inside Flatpak runtimes. Optional: without it the
# shader manager falls back to the in-overlay file browser.
gio_dep = dependency('gio-2.0', required: false)
vkhook_deps = [x11_dep, xi_dep, xtst_dep, wayland_client_dep, xkbcommon_dep, reshade_dep] + dlss5_dependencies
vkhook_cpp_args = ['-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES'] + dlss5_args
if gio_dep.found()
    vkhook_deps += gio_dep
    vkhook_cpp_args += '-DVKHOOK_HAVE_GIO=1'
endif

vkhook_lib = shared_library('vkhook',
    VkHook_src, overlay_src, imgui_src, shader_include, dlss5_sources,
    link_depends : dlss5_assets,
    link_with: [keyboard_input_x11_lib, mouse_input_lib, input_blocker_lib,
                x11_display_lib, mouse_input_x11_lib,
                wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib,
                wayland_interpose_lib],
    include_directories : [VkHook_include_path, imgui_inc, overlay_inc, effects_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, shader_asset_inc, source_inc],
    cpp_args : vkhook_cpp_args,
    dependencies : vkhook_deps,
    gnu_symbol_visibility: 'hidden',
    install : true,
    install_dir : lib_dir)

shader_check = executable('vkhook-shader-check',
    'effects/shader_check.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    'vendor/reshade/effect_codegen_spirv.cc',
    'vendor/reshade/effect_expression.cc',
    'vendor/reshade/effect_lexer.cc',
    'vendor/reshade/effect_parser.cc',
    'vendor/reshade/effect_preprocessor.cc',
    'vendor/reshade/effect_symbol_table.cc',
    'render/vk_handle.cc',
    'core/config/config.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/ini.cc',
    'core/logger.cc',
    'core/util.cc',
    include_directories : [VkHook_include_path, imgui_inc, overlay_inc, effects_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, source_inc],
    dependencies : [reshade_dep],
    install : true)

spirv_validator = find_program('spirv-val', native : true, required : false)
if spirv_validator.found()
    foreach fixture : ['ScalarTextureFetch', 'OutParameterConversion']
        test(fixture + ' SPIR-V', find_program('python3', native : true),
            args : [files('tests/test_shader_spirv.py'), shader_check.full_path(),
                    spirv_validator.full_path(), fixture + '.fx'],
            depends : shader_check)
    endforeach
endif

config_serializer_tests = executable('vkhook-config-serializer-tests',
    'tests/config_serializer_test.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/config.cc',
    'core/config/ini.cc',
    'core/logger.cc',
    'core/util.cc',
    include_directories : [core_inc, render_inc, effects_inc, effects_params_inc, vendor_inc, source_inc, imgui_inc])
test('config serializer', config_serializer_tests)

# The variable-font extension to stb_truetype is pure but invisible to CI: a bad
# download or a gvar/HVAR regression would only show as wrong-looking text. This
# links the header directly and checks the axes, outlines and advances.
variable_font_tests = executable('vkhook-variable-font-tests',
    'tests/variable_font_test.cc',
    include_directories : [imgui_inc])
test('variable font', variable_font_tests)

# The deferred destroy queue's release order is load-bearing for driver
# stability and impossible to observe without a real GPU, so it is tested
# directly rather than only through the smoke test.
vk_handle_tests = executable('vkhook-vk-handle-tests',
    'render/vk_handle_test.cc',
    'render/vk_handle.cc',
    'core/logger.cc',
    include_directories : [core_inc, render_inc, source_inc])
test('vk handle ordering', vk_handle_tests)

# X11 input blocking: notification hitboxes remain non-modal, but an interactive
# menu owns pointer and keyboard for its whole lifetime. Held keys are handed
# back on close. Uses the real blocker and synthesises input on a display:
# opt in with VKHOOK_X11_INPUT_TEST=1.
libdl_dep = meson.get_compiler('cpp').find_library('dl', required : false)
x11_input_handoff_tests = executable('vkhook-x11-input-handoff-tests',
    'tests/x11_input_handoff_test.cc',
    dependencies : [x11_dep, xtst_dep, libdl_dep],
    include_directories : [source_inc])
test('x11 input handoff', x11_input_handoff_tests, args: [vkhook_lib.full_path()])

# DepthCopyState serializes the CmdEndRenderPass* -> QueueSubmit handoff and the
# copy ring against DestroyDevice. A missing lock only shows as an intermittent
# device loss under load, so the state is hammered directly here.
threads_dep = dependency('threads')
depth_copy_state_tests = executable('vkhook-depth-copy-state-tests',
    'render/depth_copy_state_test.cc',
    dependencies : [threads_dep],
    include_directories : [render_inc, source_inc])
test('depth copy state', depth_copy_state_tests)

if dlss5_available
    dlss5_device_tests = executable('vkhook-dlss5-device-tests',
        'tests/dlss5_device_test.cc',
        'render/dlss5_device.cc',
        dlss5_chain,
        dependencies : [opendlss_nr_dep],
        include_directories : [core_inc, render_inc, source_inc])
    test('DLSS5 device feature preservation', dlss5_device_tests)
    dlss5_temporal_gpu = executable('vkhook-dlss5-temporal-gpu-smoke',
        'tests/dlss5_temporal_gpu_smoke.cc',
        'render/dlss5_device.cc',
        'render/dlss5_optical_flow.cc',
        dlss5_chain,
        dependencies : [opendlss_nr_dep, libdl_dep],
        include_directories : [core_inc, render_inc, effects_inc, source_inc],
        link_depends : dlss5_assets)
    test('DLSS5 optical-flow temporal GPU', dlss5_temporal_gpu,
        args : [opendlss_nr_build_shader_dir], timeout : 120)
    # GPU timing and head parity of the network alone: see docs/WORKFLOWS.md.
    executable('vkhook-dlss5-bench', 'tests/dlss5_network_bench.cc',
        dependencies : [opendlss_nr_dep, libdl_dep],
        link_depends : dlss5_assets)
endif

# Link the production recorder and fake only Vulkan dispatch. Section GC keeps
# unrelated command-buffer allocation/effect code out of the headless executable.
depth_snapshot_scope_tests = executable('vkhook-depth-snapshot-scope-tests',
    'render/depth_snapshot_scope_test.cc',
    'render/command_buffer.cc',
    'render/format.cc',
    'render/image_view.cc',
    'core/logger.cc',
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    dependencies : [threads_dep],
    include_directories : [core_inc, render_inc, effects_inc, source_inc])
test('depth snapshot scope', depth_snapshot_scope_tests,
    env : ['VKHOOK_LOG_LEVEL=none'])

# The compile cache is what stops a chain reload from re-parsing every ReShade
# effect. Its keying and bound are the contract, and a missing lock would only
# show as a rare corrupt module under load, so it is hammered directly.
# The explicit-sync gate's hold-and-release ordering is invisible until a
# compositor kills the connection over a racing commit, so its state machine
# is driven directly with threads.
wayland_syncobj_gate_tests = executable('vkhook-wayland-syncobj-gate-tests',
    'tests/wayland_syncobj_gate_test.cc',
    'input/wayland_syncobj_gate.cc',
    'core/logger.cc',
    dependencies : [threads_dep],
    include_directories : [core_inc, input_inc, source_inc])
test('wayland syncobj gate', wayland_syncobj_gate_tests)

# The marshal patcher parses live ELF state; reading one dynamic-section
# pointer with the wrong convention took the whole client down once.
wayland_interpose_patch_tests = executable('vkhook-wayland-interpose-patch-tests',
    'tests/wayland_interpose_patch_test.cc',
    'input/wayland_interpose_patch.cc',
    'core/logger.cc',
    dependencies : [threads_dep, wayland_client_dep],
    include_directories : [core_inc, input_inc, source_inc])
test('wayland interpose patch', wayland_interpose_patch_tests)

# The parameter parse and the runtime compile share one preprocess setup. This
# drives the parse with extents, user macros and a failing shader to check that
# macro-gated uniforms follow their macro and that compiler failures come back
# as errors.
reshade_compile_tests = executable('vkhook-reshade-compile-tests',
    'effects/reshade_compile_test.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    'effects/reshade_module_io.cc',
    'vendor/reshade/effect_codegen_spirv.cc',
    'vendor/reshade/effect_expression.cc',
    'vendor/reshade/effect_lexer.cc',
    'vendor/reshade/effect_parser.cc',
    'vendor/reshade/effect_preprocessor.cc',
    'vendor/reshade/effect_symbol_table.cc',
    'render/vk_handle.cc',
    'core/config/config.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/ini.cc',
    'core/logger.cc',
    'core/util.cc',
    include_directories : [VkHook_include_path, imgui_inc, overlay_inc, effects_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, source_inc],
    dependencies : [reshade_dep])
test('reshade compile setup', reshade_compile_tests,
    args : [meson.current_source_dir() / 'tests/fixtures'],
    env : ['VKHOOK_LOG_LEVEL=none'])

# Texture files reach the GPU through this conversion; its sizes decide how
# many bytes the upload reads, so overflows are checked here without a device.
texture_convert_tests = executable('vkhook-texture-convert-tests',
    'effects/texture_convert_test.cc',
    'effects/texture_convert.cc',
    'vendor/stb/stb_image_resize.c',
    include_directories : [render_inc, effects_inc, stb_inc, source_inc])
test('texture convert', texture_convert_tests)

compile_cache_tests = executable('vkhook-compile-cache-tests',
    'effects/compile_cache_test.cc',
    'effects/shader_cache.cc',
    'effects/reshade_module_io.cc',
    'core/config/persist.cc',
    'core/logger.cc',
    dependencies : [threads_dep],
    include_directories : [core_inc, effects_inc, effects_params_inc, vendor_inc, include_directories('vendor/reshade'), source_inc])
test('compile cache', compile_cache_tests)

# Uniform ring layout, descriptor budgets, the forwarding decision, the shared
# render pass description and the batched texture upload, all headless.
effect_plan_tests = executable('vkhook-effect-plan-tests',
    'effects/effect_plan_test.cc',
    'effects/texture_upload.cc',
    'effects/effect_renderpass.cc',
    'render/buffer.cc',
    'render/memory.cc',
    'render/image.cc',
    'render/format.cc',
    'render/vk_handle.cc',
    'core/logger.cc',
    include_directories : [core_inc, render_inc, effects_inc, effects_params_inc, vendor_inc, include_directories('vendor/reshade'), source_inc])
test('effect plan', effect_plan_tests)

reshade_texture_pool_tests = executable('vkhook-reshade-texture-pool-tests',
    'effects/reshade_texture_pool_test.cc',
    'effects/reshade_texture_pool.cc',
    'render/format.cc',
    'render/vk_handle.cc',
    'core/logger.cc',
    include_directories : [core_inc, render_inc, effects_inc, source_inc])
test('reshade texture pool', reshade_texture_pool_tests)

# Temporal ReShade shaders may declare framecount as float, int or uint.
reshade_uniform_tests = executable('vkhook-reshade-uniform-tests',
    'effects/reshade_uniforms_test.cc',
    'effects/reshade_uniforms.cc',
    'core/logger.cc',
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    include_directories : [core_inc, effects_inc, vendor_inc, source_inc])
test('reshade uniform sources', reshade_uniform_tests)

# The one INI tokenizer, the bool parser and the persistence queue's ordering
# rules (coalescing, delete-vs-autosave, shutdown) are pure and headless.
persist_tests = executable('vkhook-persist-tests',
    'tests/persist_test.cc',
    'core/config/persist.cc',
    'core/config/settings_manager.cc',
    'core/config/ini.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/config.cc',
    'core/logger.cc',
    'core/util.cc',
    dependencies : [threads_dep],
    include_directories : [core_inc, render_inc, effects_inc, effects_params_inc, vendor_inc, source_inc, imgui_inc])
test('persistence queue', persist_tests)

# the numeric field editors parse with std::from_chars; the grammar is pinned here
field_parse_tests = executable('vkhook-field-parse-tests',
    'tests/field_parse_test.cc',
    include_directories : [include_directories('overlay/params')])
test('field parse', field_parse_tests)
