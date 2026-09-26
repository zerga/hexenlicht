# Build-time GLSL -> SPIR-V compilation for the Hexenlicht renderer.
#
# hexenlicht_add_shaders(<target> <shader sources...>)
#
# Compiles every source with glslangValidator (from the Vulkan SDK) into
# ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/<file name>.spv, next to the
# executable, where the engine loads them at runtime (vk_shader.c).
# The shader stage is taken from the file extension (.vert, .frag, .comp),
# except that Quake II RTX's ray generation shaders (.rgen) are compiled as
# compute shaders with KHR_RAY_QUERY defined: Hexenlicht traces with ray
# queries only (docs/hexenlicht/Q2RTX.md). Files included with #include are tracked
# through glslang's depfile, so editing an include rebuilds its users.
# AMD's FSR 1 headers (libs/fsr1) are on the include path for the FSR
# shaders.
# Debug builds embed debug information (-g) for RenderDoc / Nsight.
# VKPT_SHADER is defined in every shader, as in Quake II RTX's build: the
# headers shared with C use it to tell shaders from C.

set(HEXENLICHT_SHADER_DIR ${CMAKE_SOURCE_DIR}/engine/hexenlicht/shaders)

function(hexenlicht_add_shaders target)
	set(out_dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
	set(outputs)
	foreach(src IN LISTS ARGN)
		get_filename_component(name ${src} NAME)
		set(out ${out_dir}/${name}.spv)
		set(stage_args)
		if(name MATCHES "\\.rgen$")
			set(stage_args -S comp -DKHR_RAY_QUERY)
		endif()
		add_custom_command(
			OUTPUT ${out}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
			COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
				-V --target-env vulkan1.3
				${stage_args}
				-DVKPT_SHADER
				$<$<CONFIG:Debug>:-g>
				-I${HEXENLICHT_SHADER_DIR}
				-I${CMAKE_SOURCE_DIR}/libs/fsr1
				--depfile ${out}.d
				-o ${out} ${src}
			MAIN_DEPENDENCY ${src}
			DEPFILE ${out}.d
			COMMENT "Compiling shader ${name}"
			COMMAND_EXPAND_LISTS
			VERBATIM
		)
		list(APPEND outputs ${out})
	endforeach()
	add_custom_target(${target} ALL DEPENDS ${outputs} SOURCES ${ARGN})
endfunction()
