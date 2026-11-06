# The engine's sources, from SConscript.core.
function(et_prefix out dir)
    set(r "")
    foreach(f ${ARGN})
        list(APPEND r ${ET_SRC}/${dir}/${f})
    endforeach()
    set(${out} ${r} PARENT_SCOPE)
endfunction()

et_prefix(ET_SERVER server sv_bot.c sv_ccmds.c sv_client.c sv_game.c sv_init.c sv_main.c sv_net_chan.c sv_snapshot.c sv_world.c)
et_prefix(ET_QCOMMON qcommon cm_load.c cm_patch.c cm_polylib.c cm_test.c cm_trace.c cmd.c common.c cvar.c files.c huffman.c
    md4.c md5.c msg.c net_chan.c unzip.c vm.c vm_interpreted.c vm_static.c sys_static_modules.c)
et_prefix(ET_SHARED game q_shared.c q_math.c)
et_prefix(ET_BOTLIB botlib be_aas_bspq3.c be_aas_cluster.c be_aas_debug.c be_aas_entity.c be_aas_file.c be_aas_main.c
    be_aas_move.c be_aas_optimize.c be_aas_reach.c be_aas_route.c be_aas_routealt.c be_aas_routetable.c be_aas_sample.c
    be_ai_char.c be_ai_chat.c be_ai_gen.c be_ai_goal.c be_ai_move.c be_ai_weap.c be_ai_weight.c be_ea.c be_interface.c
    l_crc.c l_libvar.c l_log.c l_memory.c l_precomp.c l_script.c l_struct.c)
et_prefix(ET_JPEG jpeg-6 jcapimin.c jchuff.c jcinit.c jccoefct.c jccolor.c jfdctflt.c jcdctmgr.c jcphuff.c jcmainct.c jcmarker.c
    jcmaster.c jcomapi.c jcparam.c jcprepct.c jcsample.c jdapimin.c jdapistd.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c
    jdhuff.c jdinput.c jdmainct.c jdmarker.c jdmaster.c jdpostct.c jdsample.c jdtrans.c jerror.c jidctflt.c jmemmgr.c
    jmemnobs.c jutils.c)
et_prefix(ET_RENDERER renderer tr_animation_mdm.c tr_animation_mds.c tr_backend.c tr_bsp.c tr_cmds.c tr_cmesh.c tr_curve.c
    tr_decals.c tr_flares.c tr_font.c tr_image.c tr_init.c tr_light.c tr_main.c tr_marks.c tr_mesh.c tr_model.c tr_noise.c
    tr_scene.c tr_shade.c tr_shade_calc.c tr_shader.c tr_shadows.c tr_sky.c tr_surface.c tr_world.c)
et_prefix(ET_CLIENT client cl_cgame.c cl_cin.c cl_console.c cl_input.c cl_keys.c cl_main.c cl_net_chan.c cl_parse.c cl_scrn.c
    cl_ui.c snd_adpcm.c snd_dma.c snd_mem.c snd_mix.c snd_wavelet.c)
et_prefix(ET_SPLINES splines math_angles.cpp math_matrix.cpp math_quaternion.cpp math_vector.cpp q_parse.cpp q_shared.c
    splines.cpp util_str.cpp)
