# Build-time GLSL -> SPIR-V compilation for the Hexenlicht renderer.
#
# hexenlicht_add_shaders(<target> <shader sources...>)
#
# Compiles every source with glslangValidator (from the Vulkan SDK) into
# ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/<file name>.spv, next to the
# executable, where the engine loads them at runtime (vk_shader.c).
# The shader stage is taken from the file extension (.vert, .frag, .comp,
# .rgen, .rchit, .rmiss, ...). Files included with #include are tracked
# through glslang's depfile, so editing an include rebuilds its users.
# Debug builds embed debug information (-g) for RenderDoc / Nsight.

set(HEXENLICHT_SHADER_DIR ${CMAKE_SOURCE_DIR}/engine/hexenlicht/shaders)

function(hexenlicht_add_shaders target)
	set(out_dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
	set(outputs)
	foreach(src IN LISTS ARGN)
		get_filename_component(name ${src} NAME)
		set(out ${out_dir}/${name}.spv)
		add_custom_command(
			OUTPUT ${out}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
			COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
				-V --target-env vulkan1.3
				$<$<CONFIG:Debug>:-g>
				-I${HEXENLICHT_SHADER_DIR}
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
