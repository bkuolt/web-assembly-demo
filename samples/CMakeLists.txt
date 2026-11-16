function(bgl_add_sample name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE bgl::engine)
    # Same directory as libbgl_*.so so that $ORIGIN RPATH resolves
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

bgl_add_sample(sample_01_hello_engine 01_hello_engine/main.cpp)
bgl_add_sample(sample_02_entities_and_lights 02_entities_and_lights/main.cpp)
bgl_add_sample(sample_03_sprites_and_billboards 03_sprites_and_billboards/main.cpp)
