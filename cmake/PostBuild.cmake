# =============================================================================
# PostBuild.cmake - Post-Build Artifact Generation Pipeline
# =============================================================================

include_guard(GLOBAL)

# =============================================================================
# Post-Build Commands
# =============================================================================
function(pir_add_postbuild target_name)
    set(_out "${PIR_OUTPUT_DIR}/output")

    # Skip PIC artifacts for debug optimization levels (O0/Og) since they
    # produce data sections that break position-independence. Supporting PIC in
    # debug builds is technically possible but requires significant additional
    # work in the pic-transform pass and compiler flag tuning.
    set(_is_debug_opt FALSE)
    if(PIR_OPT_LEVEL MATCHES "^O[0g]$")
        set(_is_debug_opt TRUE)
    endif()

    if(_is_debug_opt)
        pir_log_verbose("Post-build pipeline for ${target_name} (debug -${PIR_OPT_LEVEL}, PIC artifacts skipped):")
    else()
        pir_log_verbose("Post-build pipeline for ${target_name}:")
        pir_log_verbose("  1. Extract .text section -> .bin")
        pir_log_verbose("  2. Base64 encode -> .b64.txt")
        pir_log_verbose("  3. Verify PIC mode (no data sections)")
    endif()
    pir_log_debug("Post-build input: ${_out}${PIR_EXT}")

    # Patch ELF EI_OSABI when required (e.g. Solaris needs ELFOSABI_SOLARIS)
    set(_osabi_cmd)
    if(DEFINED PIR_ELF_OSABI)
        pir_log_verbose("Post-build: ELF OSABI patch (value=${PIR_ELF_OSABI})")
        set(_osabi_cmd
            COMMAND ${CMAKE_COMMAND}
                -DELF_FILE="${_out}${PIR_EXT}"
                -DOSABI_VALUE=${PIR_ELF_OSABI}
                -P "${PIR_ROOT_DIR}/cmake/scripts/PatchElfOSABI.cmake"
        )
    endif()

    # PIC artifact commands: .bin extraction, base64, and verification
    # Skipped entirely for debug optimization levels (O0/Og)
    set(_pic_cmds)
    if(NOT _is_debug_opt)
        set(_pic_cmds
            COMMAND ${CMAKE_COMMAND}
                -DINPUT_FILE="${_out}${PIR_EXT}"
                -DOUTPUT_DIR="${PIR_OUTPUT_DIR}"
                -P "${PIR_ROOT_DIR}/cmake/scripts/ExtractBinary.cmake"
            COMMAND ${CMAKE_COMMAND}
                -DPIC_FILE="${_out}.bin"
                -DBASE64_FILE="${_out}.b64.txt"
                -P "${PIR_ROOT_DIR}/cmake/scripts/Base64Encode.cmake"
            COMMAND ${CMAKE_COMMAND}
                -DMAP_FILE="${PIR_MAP_FILE}"
                -P "${PIR_ROOT_DIR}/cmake/scripts/VerifyPICMode.cmake"
        )

        # Polymorphic instruction analysis (when tool is available)
        if(POLY_TRANSFORM_EXECUTABLE)
            pir_log_verbose("  4. Instruction set analysis (poly-transform)")
            # Generate disassembly and run analysis
            # Note: verification is informational only — does not fail the build
            find_program(_ply_objdump llvm-objdump)
            if(_ply_objdump)
                list(APPEND _pic_cmds
                    COMMAND ${CMAKE_COMMAND}
                        -DPLY_OBJDUMP=${_ply_objdump}
                        -DPLY_EXECUTABLE=${POLY_TRANSFORM_EXECUTABLE}
                        -DPLY_INPUT="${_out}${PIR_EXT}"
                        -DPLY_DISASM="${_out}.disasm"
                        -DPLY_ARCH=${PIR_ARCH}
                        -DPLY_SEED=${POLY_TRANSFORM_SEED}
                        -P "${PIR_ROOT_DIR}/cmake/scripts/PolyTransformAnalyze.cmake"
                )
            endif()
        endif()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PIR_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E echo "[pir] Build complete: ${_out}${PIR_EXT}"
        ${_osabi_cmd}
        ${_pic_cmds}
        COMMENT "[pir] Generating PIC artifacts..."
    )
endfunction()
