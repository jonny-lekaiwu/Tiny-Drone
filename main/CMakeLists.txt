idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES remote_id device_activation
                       LDFRAGMENTS linker_fragment.lf)
