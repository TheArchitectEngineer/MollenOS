project (vali-kernel-output)
enable_language (C)

# Configure source files
add_kernel_library (vali-core-output ""
    console.c
    log.c
)
