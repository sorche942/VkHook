subdir('vendor/reshade')

glsl_shader_src = files(
    'effects/shaders/glsl/depth_resolve.frag.glsl',
    'effects/shaders/glsl/full_screen_triangle.vert.glsl',
)
glsl_compiler = find_program('glslangValidator')
glsl_generator = generator(glsl_compiler,
    output: ['@BASENAME@.h'],
    arguments: ['-V', '-x', '@INPUT@', '-o', '@OUTPUT@'])
# the multisampled depth resolve is the same source built with -DMSAA
depth_resolve_msaa_spv = custom_target('depth_resolve_msaa.frag.h',
    input : files('effects/shaders/glsl/depth_resolve.frag.glsl'),
    output : 'depth_resolve_msaa.frag.h',
    command : [glsl_compiler, '-V', '-x', '-DMSAA', '@INPUT@', '-o', '@OUTPUT@'])
shader_include = [glsl_generator.process(glsl_shader_src), depth_resolve_msaa_spv]

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
    # VkHook's own DLSS5 compute shaders are built next to the vendored network kernels
    # so a build tree has a single shader directory; the vendor file only sees these lists.
    opendlss_nr_extra_shaders = files(
        'effects/shaders/glsl/dlss5_preprocess.comp',
        'effects/shaders/glsl/dlss5_composite.comp',
        'effects/shaders/glsl/dlss5_reproject.comp',
        'effects/shaders/glsl/dlss5_luma.comp',
    )
    opendlss_nr_extra_shader_deps = files(
        'effects/shaders/glsl/dlss5_common.glsl',
        'effects/shaders/glsl/dlss5_debug.glsl',
    )
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
    'overlay/overlay_ui.cc',
    'overlay/overlay_renderer.cc',
    'overlay/commands.cc',
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

# Config, logging, util and the preset model: everything the layer and the tests
# share and that needs no Vulkan. Built once as vkhook_core; the tests link it
# rather than recompiling the same files.
core_src = [
    'core/logger.cc',
    'core/util.cc',
    'core/config/ini.cc',
    'core/config/config.cc',
    'core/config/config_serializer.cc',
    'core/config/config_serializer_profile.cc',
    'core/config/persist.cc',
    'core/config/store.cc',
    'core/config/settings_schema.cc',
    'core/config/migrate.cc',
    'control/preset_model.cc',
    'effects/effect_id.cc',
]

VkHook_src = [
    'layer/entry.cc',
    'layer/registry.cc',
    'layer/instance.cc',
    'layer/device.cc',
    'layer/device_plan.cc',
    'layer/pipeline_cache.cc',
    'gpu/queue.cc',
    'gpu/retire.cc',
    'gpu/retire_device.cc',
    'gpu/timeline.cc',
    'present/swapchain.cc',
    'present/present.cc',
    'present/present_mode.cc',
    'present/submit.cc',
    'depth/capture.cc',
    'depth/hooks.cc',
    'depth/tracker.cc',
    'control/frontend.cc',
    'control/commands_apply.cc',
    'control/preset_import.cc',
    'diag/gpu_crash.cc',
    'render/buffer.cc',
    'render/command_buffer.cc',
    'render/descriptor_set.cc',
    'effects/effect_registry.cc',
    'effects/shader_catalog.cc',
    'effects/effect_reshade.cc',
    'effects/reshade_texture_pool.cc',
    'effects/effect_transfer.cc',
    'effects/effect_renderpass.cc',
    'effects/texture_upload.cc',
    'effects/reshade_parser.cc',
    'effects/technique_scan.cc',
    'effects/compiler.cc',
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
    'render/logical_swapchain.cc',
    'render/memory.cc',
    'effects/reshade_uniforms.cc',
    'render/sampler.cc',
    'render/shader.cc',
    'vendor/stb/stb_image.c',
    'vendor/stb/stb_image_resize.c',
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
threads_dep = dependency('threads')
libdl_dep = cpp.find_library('dl', required : false)

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

# The binary reports its own version, so it must not depend on its filename or
# on the setup script dropping a version file next to the config. The root
# VERSION file is the only place the number is written (project(), the layer
# manifest and the Nix flake all read it).
conf_version = configuration_data()
conf_version.set_quoted('VKHOOK_VERSION', vkhook_version)

configure_file(output: 'version.hh',
               configuration : conf_version)

imgui_inc = include_directories('vendor/imgui')
core_inc = include_directories('core', 'core/config')
input_inc = include_directories('input')
render_inc = include_directories('render')
effects_inc = include_directories('effects')
# One include set for the layer, the core library and every test: the tree is a
# single flat namespace of headers, so per-target lists only drifted.
vkhook_inc = include_directories(
    '.', 'core', 'core/config', 'input', 'render', 'effects', 'effects/params', 'effects/shaders',
    'overlay', 'overlay/params', 'vendor', 'vendor/imgui', 'vendor/reshade', 'vendor/stb')

# The XDG desktop portal is reached over the session bus through GIO, which is
# present on the desktop and inside Flatpak runtimes. Optional: without it the
# shader manager falls back to the in-overlay file browser.
gio_dep = dependency('gio-2.0', required: false)
core_deps = [threads_dep]
core_cpp_args = []
if gio_dep.found()
    core_deps += gio_dep
    core_cpp_args += '-DVKHOOK_HAVE_GIO=1'
endif

vkhook_core_lib = static_library('vkhook_core',
    core_src,
    include_directories : vkhook_inc,
    cpp_args : core_cpp_args,
    dependencies : core_deps,
    gnu_symbol_visibility : 'hidden')
vkhook_core_dep = declare_dependency(
    link_with : vkhook_core_lib,
    include_directories : vkhook_inc,
    dependencies : core_deps)

# X11/X.h library contains many definitions (e.g. None)
# which conflicts with the rest codebase with with Meson --unity=on option
keyboard_input_x11_lib = static_library('keyboard_input_x11', 'input/keyboard_input_x11.cc', include_directories: core_inc)
mouse_input_lib = static_library('mouse_input', 'input/mouse_input.cc', include_directories: core_inc)
input_blocker_lib = static_library('input_blocker', 'input/input_blocker.cc', include_directories: core_inc)

# X11 input backend (X11Input). The class is implemented across three
# translation units so that its connection, keyboard and mouse halves stay
# separate; each session owns one connection. Kept out of the unity-compiled
# core because X11/X.h defines symbols (None, ...) that collide with the codebase.
x11_display_lib = static_library('x11_display',
    'input/x11_display.cc',
    include_directories: core_inc)
mouse_input_x11_lib = static_library('mouse_input_x11',
    'input/mouse_input_x11.cc',
    include_directories: core_inc)

# Wayland input backend (WaylandInput) — built as static libs to isolate wayland-client
# headers. wayland_input_common holds the seat/queue/capture surface shared by keyboard and mouse.
wayland_input_common_lib = static_library('wayland_input_common',
    'input/wayland_input_common.cc',
    viewporter_code, viewporter_client_h,
    dependencies: [wayland_client_dep],
    include_directories: core_inc)
keyboard_input_wayland_lib = static_library('keyboard_input_wayland',
    'input/keyboard_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    include_directories: core_inc)
mouse_input_wayland_lib = static_library('mouse_input_wayland',
    'input/mouse_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
    include_directories: core_inc)
# Explicit-sync commit gate for the wayland interpose: keeps a foreign window
# commit out of an open linux-drm-syncobj point sequence.
wayland_syncobj_gate_lib = static_library('wayland_syncobj_gate',
    'input/wayland_syncobj_gate.cc',
    include_directories: core_inc)
# Wayland event interposition — wraps game's pointer/keyboard listeners to
# suppress events when overlay has input blocked. Needs default visibility
# for the wl_proxy_add_listener symbol override.
wayland_interpose_lib = static_library('wayland_interpose',
    'input/wayland_interpose.cc', 'input/wayland_interpose_patch.cc',
    link_with: [wayland_syncobj_gate_lib],
    dependencies: [wayland_client_dep],
    c_args: ['-fvisibility=default'],
    cpp_args: ['-fvisibility=default'],
    include_directories: core_inc)

# InputSession and the free-function facade over the current session. Includes no
# X11 or wayland-client header; it reaches the backends through x11_input.hh and
# wayland_input.hh, whose libraries the final link supplies.
input_session_lib = static_library('input_session',
    'input/input_session.cc', 'input/wayland_display.cc',
    include_directories: core_inc)

# Every input backend library, in link order, for targets that use InputSession.
input_backend_libs = [input_session_lib, input_blocker_lib, mouse_input_lib,
    x11_display_lib, keyboard_input_x11_lib, mouse_input_x11_lib,
    wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib]
input_backend_deps = [x11_dep, xi_dep, xtst_dep, wayland_client_dep, xkbcommon_dep]

vkhook_deps = [vkhook_core_dep, reshade_dep] + input_backend_deps + dlss5_dependencies
vkhook_cpp_args = ['-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES'] + dlss5_args + core_cpp_args

vkhook_lib = shared_library('vkhook',
    VkHook_src, overlay_src, imgui_src, shader_include, dlss5_sources,
    link_depends : dlss5_assets,
    link_with: [keyboard_input_x11_lib, mouse_input_lib, input_blocker_lib, input_session_lib,
                x11_display_lib, mouse_input_x11_lib,
                wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib,
                wayland_interpose_lib],
    include_directories : vkhook_inc,
    cpp_args : vkhook_cpp_args,
    dependencies : vkhook_deps,
    gnu_symbol_visibility: 'hidden',
    install : true,
    install_dir : lib_dir)

shader_check = executable('vkhook-shader-check',
    'effects/shader_check.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    dependencies : [vkhook_core_dep, reshade_dep],
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
    dependencies : vkhook_core_dep)
test('config serializer', config_serializer_tests, suite : ['threaded'])

# The preset model (parse/serialize of ReShade presets plus the VkHook sidecar) is
# pure: golden files in tests/fixtures/presets pin the on-disk format.
preset_model_tests = executable('vkhook-preset-model-tests',
    'tests/preset_model_test.cc',
    dependencies : vkhook_core_dep)
test('preset model', preset_model_tests,
    args : [meson.current_source_dir() / 'tests/fixtures'])

# The technique scan: a pure text scan of a .fx file for its `technique` names.
technique_scan_tests = executable('vkhook-technique-scan-tests',
    'effects/technique_scan_test.cc',
    'effects/technique_scan.cc',
    include_directories : vkhook_inc)
test('technique scan', technique_scan_tests)

# X11 input blocking: notification hitboxes remain non-modal, but an interactive
# menu owns pointer and keyboard for its whole lifetime. Held keys are handed
# back on close. Uses the real blocker and synthesises input on a display:
# opt in with VKHOOK_X11_INPUT_TEST=1.
x11_input_handoff_tests = executable('vkhook-x11-input-handoff-tests',
    'tests/x11_input_handoff_test.cc',
    link_with : input_backend_libs,
    dependencies : [vkhook_core_dep] + input_backend_deps)
test('x11 input handoff', x11_input_handoff_tests)



# RetireQueue + Timeline (formal/VkHookFormal/RetireQueue.lean): seeded random
# action sequences against a ground-truth GPU model, no Vulkan device needed.
retire_tests = executable('vkhook-retire-tests',
    'gpu/retire_test.cc',
    'gpu/retire.cc',
    'gpu/timeline.cc',
    dependencies : vkhook_core_dep)
test('retire queue', retire_tests, suite : ['threaded'])

if dlss5_available
    dlss5_device_tests = executable('vkhook-dlss5-device-tests',
        'tests/dlss5_device_test.cc',
        'render/dlss5_device.cc',
        dlss5_chain,
        dependencies : [vkhook_core_dep, opendlss_nr_dep])
    test('DLSS5 device feature preservation', dlss5_device_tests)
    dlss5_temporal_gpu = executable('vkhook-dlss5-temporal-gpu-smoke',
        'tests/dlss5_temporal_gpu_smoke.cc',
        'render/dlss5_device.cc',
        'render/dlss5_optical_flow.cc',
        'gpu/queue.cc',
        'gpu/timeline.cc',
        dlss5_chain,
        dependencies : [vkhook_core_dep, opendlss_nr_dep, libdl_dep],
        link_depends : dlss5_assets)
    test('DLSS5 optical-flow temporal GPU', dlss5_temporal_gpu,
        args : [opendlss_nr_build_shader_dir], timeout : 120)
    # GPU timing and head parity of the network alone: see docs/WORKFLOWS.md.
    executable('vkhook-dlss5-bench', 'tests/dlss5_network_bench.cc',
        dependencies : [opendlss_nr_dep, libdl_dep],
        link_depends : dlss5_assets)
endif

# Pure device/instance create planning (AGENTS.md invariant 9): no GPU needed.
device_plan_sources = ['tests/device_plan_test.cc', 'layer/device_plan.cc']
device_plan_deps = [vkhook_core_dep]
if dlss5_available
    device_plan_sources += ['render/dlss5_device.cc', dlss5_chain]
    device_plan_deps += opendlss_nr_dep
endif
device_plan_tests = executable('vkhook-device-plan-tests',
    device_plan_sources,
    cpp_args : dlss5_args,
    dependencies : device_plan_deps)
test('device plan', device_plan_tests)

# Present mode chooser and semaphore accounting, driven through the production
# submitPresentBatches with a fake queue dispatch.
present_mode_tests = executable('vkhook-present-mode-tests',
    'tests/present_mode_test.cc',
    'present/present_mode.cc',
    'gpu/queue.cc',
    'gpu/timeline.cc',
    dependencies : vkhook_core_dep)
test('present modes', present_mode_tests,
    env : ['VKHOOK_LOG_LEVEL=none'],
    suite : ['threaded'])

# SwapchainShared::release destroys by VkObjectType, not by C++ handle type (all
# non-dispatchable handles are one uint64_t on i686): the 32-bit layer crashed at the
# first swapchain recreation. Runs in the i686 build too.
swapchain_release_tests = executable('vkhook-swapchain-release-tests',
    'tests/swapchain_release_test.cc',
    'render/logical_swapchain.cc',
    'gpu/retire_device.cc',
    'gpu/retire.cc',
    'gpu/timeline.cc',
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    dependencies : vkhook_core_dep)
test('swapchain release', swapchain_release_tests,
    env : ['VKHOOK_LOG_LEVEL=none'])

# The depth tracker's contracts (candidate selection, layout tracking through
# barriers, storage lifetime against recorded command buffers, handle reuse)
# against a fake Vulkan dispatch; no device needed.
depth_tracker_tests = executable('vkhook-depth-tracker-tests',
    'depth/tracker_test.cc',
    'depth/tracker.cc',
    'render/format.cc',
    'render/image_view.cc',
    'render/memory.cc',
    'gpu/retire.cc',
    'gpu/timeline.cc',
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    dependencies : vkhook_core_dep)
test('depth tracker', depth_tracker_tests,
    env : ['VKHOOK_LOG_LEVEL=none'])

# Raw vkQueue* calls bypass QueueTable's per-queue lock (AGENTS.md invariant 1).
python3 = find_program('python3')
test('queue calls', python3,
    args : [files('tests/check_queue_calls.py'), meson.current_source_dir()])

# The explicit-sync gate's hold-and-release ordering is invisible until a
# compositor kills the connection over a racing commit, so its state machine
# is driven directly with threads.
wayland_syncobj_gate_tests = executable('vkhook-wayland-syncobj-gate-tests',
    'tests/wayland_syncobj_gate_test.cc',
    'input/wayland_syncobj_gate.cc',
    dependencies : vkhook_core_dep)
test('wayland syncobj gate', wayland_syncobj_gate_tests, suite : ['threaded'])

# The marshal patcher parses live ELF state; reading one dynamic-section
# pointer with the wrong convention took the whole client down once.
wayland_interpose_patch_tests = executable('vkhook-wayland-interpose-patch-tests',
    'tests/wayland_interpose_patch_test.cc',
    'input/wayland_interpose_patch.cc',
    dependencies : [vkhook_core_dep, wayland_client_dep])
test('wayland interpose patch', wayland_interpose_patch_tests)

# The capture subsurface must follow the game's surface when the game recreates
# it, and Wayland objects must be destroyed children-first with the event queue
# last. Both are pure state machines (capture_attachment.hh, release_stack.hh),
# driven here without a compositor.
wayland_capture_tests = executable('vkhook-wayland-capture-tests',
    'tests/wayland_capture_test.cc',
    include_directories : input_inc)
test('wayland capture attachment and teardown order', wayland_capture_tests)

# InputSession: the registry's "current session" policy, per-session blocking
# state, surface re-targeting and destruction. The interposer's two entry points
# are faked so the test records the key-withhold requests instead of linking the
# process-wide dlsym override.
input_session_tests = executable('vkhook-input-session-tests',
    'tests/input_session_test.cc',
    'input/keyboard_input.cc',
    link_with : input_backend_libs,
    dependencies : [vkhook_core_dep] + input_backend_deps)
test('input session', input_session_tests)

# WaylandInput against a real libwayland client and an in-process fake server:
# teardown order (no proxy left on the destroyed event queue), re-attaching the
# capture subsurface when the game recreates its surface, and re-initialisation.
wayland_server_dep = dependency('wayland-server', required: false)
if wayland_server_dep.found()
    wayland_input_teardown_tests = executable('vkhook-wayland-input-teardown-tests',
        'tests/wayland_input_teardown_test.cc',
        link_with : [wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib],
        dependencies : [vkhook_core_dep, wayland_client_dep, wayland_server_dep, xkbcommon_dep])
    test('wayland input teardown', wayland_input_teardown_tests)
endif

# The parameter parse and the runtime compile share one preprocess setup. This
# drives the parse with extents, user macros and a failing shader to check that
# macro-gated uniforms follow their macro and that compiler failures come back
# as errors.
reshade_compile_tests = executable('vkhook-reshade-compile-tests',
    'effects/reshade_compile_test.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    'effects/reshade_module_io.cc',
    dependencies : [vkhook_core_dep, reshade_dep])
test('reshade compile setup', reshade_compile_tests,
    args : [meson.current_source_dir() / 'tests/fixtures'],
    env : ['VKHOOK_LOG_LEVEL=none'])

# The one compile path and its worker: equivalence with the synchronous
# compile, technique selection, per-channel cancellation and shutdown.
compiler_tests = executable('vkhook-compiler-tests',
    'effects/compiler_test.cc',
    'effects/compiler.cc',
    'effects/shader_cache.cc',
    'effects/reshade_compile.cc',
    'effects/reshade_module_io.cc',
    'vendor/reshade/effect_codegen_spirv.cc',
    'vendor/reshade/effect_expression.cc',
    'vendor/reshade/effect_lexer.cc',
    'vendor/reshade/effect_parser.cc',
    'vendor/reshade/effect_preprocessor.cc',
    'vendor/reshade/effect_symbol_table.cc',
    dependencies : [vkhook_core_dep, reshade_dep])
test('effect compiler', compiler_tests,
    args : [meson.current_source_dir() / 'tests/fixtures'],
    env : ['VKHOOK_LOG_LEVEL=none'])

# EffectId text form, the shader catalog index, and the registry on real
# shaders: legacy-name migration, technique entries, macro re-parse on the worker.
effect_id_tests = executable('vkhook-effect-id-tests',
    'effects/effect_id_test.cc',
    dependencies : vkhook_core_dep)
test('effect id', effect_id_tests)

shader_catalog_tests = executable('vkhook-shader-catalog-tests',
    'effects/shader_catalog_test.cc',
    'effects/shader_catalog.cc',
    dependencies : vkhook_core_dep)
test('shader catalog', shader_catalog_tests)

effect_registry_tests = executable('vkhook-effect-registry-tests',
    'effects/effect_registry_test.cc',
    'effects/effect_registry.cc',
    'effects/shader_catalog.cc',
    'effects/compiler.cc',
    'effects/shader_cache.cc',
    'effects/reshade_parser.cc',
    'effects/reshade_compile.cc',
    'effects/reshade_module_io.cc',
    'vendor/reshade/effect_codegen_spirv.cc',
    'vendor/reshade/effect_expression.cc',
    'vendor/reshade/effect_lexer.cc',
    'vendor/reshade/effect_parser.cc',
    'vendor/reshade/effect_preprocessor.cc',
    'vendor/reshade/effect_symbol_table.cc',
    dependencies : [vkhook_core_dep, reshade_dep])
test('effect registry', effect_registry_tests,
    args : [meson.current_source_dir() / 'tests/fixtures'],
    env : ['VKHOOK_LOG_LEVEL=none'],
    timeout : 120)

# Texture files reach the GPU through this conversion; its sizes decide how
# many bytes the upload reads, so overflows are checked here without a device.
texture_convert_tests = executable('vkhook-texture-convert-tests',
    'effects/texture_convert_test.cc',
    'effects/texture_convert.cc',
    'vendor/stb/stb_image_resize.c',
    include_directories : vkhook_inc)
test('texture convert', texture_convert_tests)

# The compile cache is what stops a chain reload from re-parsing every ReShade
# effect. Its keying and bound are the contract, and a missing lock would only
# show as a rare corrupt module under load, so it is hammered directly.
compile_cache_tests = executable('vkhook-compile-cache-tests',
    'effects/compile_cache_test.cc',
    'effects/shader_cache.cc',
    'effects/reshade_module_io.cc',
    dependencies : vkhook_core_dep)
test('compile cache', compile_cache_tests, suite : ['threaded'])

# Uniform ring layout, descriptor budgets, the forwarding decision, the shared
# render pass description and the batched texture upload, all headless.
effect_plan_tests = executable('vkhook-effect-plan-tests',
    'effects/effect_plan_test.cc',
    'effects/texture_upload.cc',
    'gpu/queue.cc',
    'effects/effect_renderpass.cc',
    'render/buffer.cc',
    'render/memory.cc',
    'render/image.cc',
    'render/format.cc',
    'gpu/retire.cc',
    'gpu/retire_device.cc',
    'gpu/timeline.cc',
    dependencies : vkhook_core_dep)
test('effect plan', effect_plan_tests)

reshade_texture_pool_tests = executable('vkhook-reshade-texture-pool-tests',
    'effects/reshade_texture_pool_test.cc',
    'effects/reshade_texture_pool.cc',
    'render/format.cc',
    'gpu/retire.cc',
    'gpu/timeline.cc',
    dependencies : vkhook_core_dep)
test('reshade texture pool', reshade_texture_pool_tests)

# Temporal ReShade shaders may declare framecount as float, int or uint.
reshade_uniform_tests = executable('vkhook-reshade-uniform-tests',
    'effects/reshade_uniforms_test.cc',
    'effects/reshade_uniforms.cc',
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    dependencies : vkhook_core_dep)
test('reshade uniform sources', reshade_uniform_tests)

# The one INI tokenizer, the bool parser and the persistence queue's ordering
# rules (coalescing, delete-vs-autosave, shutdown) are pure and headless.
persist_tests = executable('vkhook-persist-tests',
    'tests/persist_test.cc',
    dependencies : vkhook_core_dep)
test('persistence queue', persist_tests, suite : ['threaded'])

# The settings schema (defaults, clamping, file round trip), the published
# snapshots and the config-directory migrations.
config_store_tests = executable('vkhook-config-store-tests',
    'tests/config_store_test.cc',
    dependencies : vkhook_core_dep)
test('config store', config_store_tests, suite : ['threaded'])

# the overlay's command queue and the SetParam value copy
overlay_commands_tests = executable('vkhook-overlay-commands-tests',
    'tests/overlay_commands_test.cc',
    'overlay/commands.cc',
    dependencies : vkhook_core_dep)
test('overlay commands', overlay_commands_tests, suite : ['threaded'])

# the numeric field editors parse with std::from_chars; the grammar is pinned here
field_parse_tests = executable('vkhook-field-parse-tests',
    'tests/field_parse_test.cc',
    include_directories : include_directories('overlay/params'))
test('field parse', field_parse_tests)

# C2 regression: a game-shaped app reads the X server's view of its window back and
# checks it follows what the app rendered while the layer swaps the swapchain images.
# It must not link libvkhook: the layer is loaded through the Vulkan loader, like a game.
present_pixels_app = executable('present-pixels',
    'tests/present_pixels.cc',
    dependencies : [dependency('vulkan'), x11_dep])
test('present pixels', find_program('python3', native : true),
    args : [files('tests/present_pixels.py'), present_pixels_app.full_path(), vkhook_lib.full_path()],
    depends : [present_pixels_app, vkhook_lib],
    is_parallel : false,
    timeout : 180,
    protocol : 'exitcode')
# A present that reports SUBOPTIMAL is a success: an app that keeps its swapchain
# (it may) must keep getting processed frames.
test('present pixels without recreate', find_program('python3', native : true),
    args : [files('tests/present_pixels.py'), present_pixels_app.full_path(), vkhook_lib.full_path(),
            '--no-recreate'],
    depends : [present_pixels_app, vkhook_lib],
    is_parallel : false,
    timeout : 180,
    protocol : 'exitcode')
