idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES remote_id
                       LDFRAGMENTS linker_fragment.lf)
