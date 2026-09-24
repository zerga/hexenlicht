# Source lists for the Hexen II engine (engine/hexen2), mirroring the
# object lists in engine/hexen2/Makefile for a Win64 build.
#
# H2_COMMON_SOURCES     - everything except the renderer (client, server,
#                         progs VM, sound, net, input, Win32 sys layer)
# H2_GL_SOURCES         - the OpenGL renderer and its Win32 video driver
# H2_WIN32_RC           - the Win32 resource script (icon, splash, strings)
#
# Keep these in sync with the Makefile when merging upstream changes.

set(H2_DIR     ${CMAKE_SOURCE_DIR}/engine/hexen2)
set(H2S_DIR    ${CMAKE_SOURCE_DIR}/engine/h2shared)
set(H2CMN_DIR  ${CMAKE_SOURCE_DIR}/common)

# Makefile: GLOBJS (SYSOBJ_GL_VID = gl_vidnt.o on win64)
set(H2_GL_SOURCES
	${H2S_DIR}/gl_refrag.c
	${H2S_DIR}/gl_rlight.c
	${H2_DIR}/gl_rmain.c
	${H2_DIR}/gl_rmisc.c
	${H2_DIR}/r_part.c
	${H2_DIR}/gl_rsurf.c
	${H2S_DIR}/gl_screen.c
	${H2S_DIR}/gl_warp.c
	${H2S_DIR}/gl_vidnt.c
	${H2S_DIR}/gl_draw.c
	${H2S_DIR}/gl_mesh.c
	${H2S_DIR}/gl_model.c
)

# Makefile: COMOBJ_SND + MUSIC_OBJS (mp3_obj = snd_mp3 with MP3LIB=mad).
# Codecs that are not enabled compile to empty translation units.
set(H2_SOUND_SOURCES
	${H2S_DIR}/snd_sys.c
	${H2S_DIR}/snd_dma.c
	${H2S_DIR}/snd_mix.c
	${H2S_DIR}/snd_mem.c
	${H2S_DIR}/bgmusic.c
	${H2S_DIR}/snd_codec.c
	${H2S_DIR}/snd_flac.c
	${H2S_DIR}/snd_wave.c
	${H2S_DIR}/snd_vorbis.c
	${H2S_DIR}/snd_opus.c
	${H2S_DIR}/snd_mp3.c
	${H2S_DIR}/snd_mp3tag.c
	${H2S_DIR}/snd_mikmod.c
	${H2S_DIR}/snd_modplug.c
	${H2S_DIR}/snd_xmp.c
	${H2S_DIR}/snd_umx.c
	${H2S_DIR}/snd_timidity.c
	${H2S_DIR}/snd_wildmidi.c
	# SYSOBJ_SND (win64)
	${H2S_DIR}/snd_win.c
	${H2S_DIR}/snd_dsound.c
)

# Makefile: COMMONOBJS with the win64 SYSOBJ_* choices
set(H2_COMMON_SOURCES
	${H2S_DIR}/in_win.c
	${H2_SOUND_SOURCES}
	${H2S_DIR}/cd_win.c
	${H2S_DIR}/midi_win.c
	${H2S_DIR}/mid2strm.c
	${H2_DIR}/net_win.c
	${H2_DIR}/net_wins.c
	${H2_DIR}/net_wipx.c
	${H2_DIR}/net_dgrm.c
	${H2_DIR}/net_loop.c
	${H2_DIR}/net_main.c
	${H2_DIR}/chase.c
	${H2_DIR}/cl_demo.c
	${H2_DIR}/cl_effect.c
	${H2_DIR}/cl_inlude.c
	${H2_DIR}/cl_input.c
	${H2_DIR}/cl_main.c
	${H2_DIR}/cl_parse.c
	${H2_DIR}/cl_string.c
	${H2_DIR}/cl_tent.c
	${H2_DIR}/cl_cmd.c
	${H2_DIR}/console.c
	${H2_DIR}/keys.c
	${H2_DIR}/menu.c
	${H2_DIR}/sbar.c
	${H2_DIR}/view.c
	${H2S_DIR}/wad.c
	${H2S_DIR}/cmd.c
	${H2CMN_DIR}/q_endian.c
	${H2S_DIR}/link_ops.c
	${H2S_DIR}/sizebuf.c
	${H2CMN_DIR}/strlcat.c
	${H2CMN_DIR}/strlcpy.c
	${H2CMN_DIR}/qsnprint.c
	${H2S_DIR}/msg_io.c
	${H2S_DIR}/common.c
	${H2S_DIR}/debuglog.c
	${H2S_DIR}/quakefs.c
	${H2CMN_DIR}/crc.c
	${H2S_DIR}/cvar.c
	${H2S_DIR}/cfgfile.c
	${H2_DIR}/host.c
	${H2_DIR}/host_cmd.c
	${H2S_DIR}/host_string.c
	${H2S_DIR}/mathlib.c
	${H2S_DIR}/pr_cmds.c
	${H2S_DIR}/pr_edict.c
	${H2S_DIR}/pr_exec.c
	${H2_DIR}/sv_effect.c
	${H2_DIR}/sv_main.c
	${H2_DIR}/sv_move.c
	${H2_DIR}/sv_phys.c
	${H2_DIR}/sv_user.c
	${H2_DIR}/world.c
	${H2S_DIR}/zone.c
	${H2S_DIR}/hashindex.c
	${H2_DIR}/sys_win.c
)

set(H2_WIN32_RC ${H2S_DIR}/win32res.rc)

# Files of the GL renderer that make no OpenGL calls and are reused by the
# hexenlicht target: model loading, alias model mesh building, particle
# simulation (r_part.c leaves out its GL drawing when HEXENLICHT is
# defined), static entity fragments, and the screen layout (gl_screen.c,
# whose screenshot command is redirected to Vulkan when HEXENLICHT is
# defined; its GL_BeginRendering/GL_Set2D/GL_EndRendering come from
# engine/hexenlicht/vk_draw.c).
set(H2_HEXENLICHT_REUSED_SOURCES
	${H2S_DIR}/gl_model.c
	${H2S_DIR}/gl_mesh.c
	${H2_DIR}/r_part.c
	${H2S_DIR}/gl_refrag.c
	${H2S_DIR}/gl_screen.c
)
