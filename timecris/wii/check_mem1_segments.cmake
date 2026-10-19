# Fails the build when a PT_LOAD segment exceeds MEM1 or lands in the MEM1/MEM2 gap.
# Expected variables:
#   READELF_TOOL: path to powerpc-eabi-readelf
#   ELF_FILE:     built ELF path

if(NOT DEFINED READELF_TOOL OR READELF_TOOL STREQUAL "")
  message(FATAL_ERROR "MEM1 check: READELF_TOOL is not set")
endif()
if(NOT EXISTS "${READELF_TOOL}")
  message(FATAL_ERROR "MEM1 check: readelf tool not found at '${READELF_TOOL}'")
endif()
if(NOT DEFINED ELF_FILE OR ELF_FILE STREQUAL "")
  message(FATAL_ERROR "MEM1 check: ELF_FILE is not set")
endif()
if(NOT EXISTS "${ELF_FILE}")
  message(FATAL_ERROR "MEM1 check: ELF file not found at '${ELF_FILE}'")
endif()

# Wii memory map (cached virtual addresses)
math(EXPR MEM1_START "0x80000000")
math(EXPR MEM1_END   "0x81800000")
math(EXPR MEM2_START "0x90000000")

execute_process(
  COMMAND "${READELF_TOOL}" -l "${ELF_FILE}"
  RESULT_VARIABLE READELF_RC
  OUTPUT_VARIABLE READELF_OUT
  ERROR_VARIABLE READELF_ERR
)
if(NOT READELF_RC EQUAL 0)
  message(FATAL_ERROR "MEM1 check: readelf failed (${READELF_RC}): ${READELF_ERR}")
endif()

string(REPLACE "\r\n" "\n" READELF_OUT "${READELF_OUT}")
string(REGEX MATCHALL "[^\n]*LOAD[^\n]*" LOAD_LINES "${READELF_OUT}")
if(NOT LOAD_LINES)
  message(FATAL_ERROR "MEM1 check: no LOAD lines found in readelf output")
endif()

foreach(load_line IN LISTS LOAD_LINES)
  string(REGEX MATCH "LOAD[ \t]+0x[0-9A-Fa-f]+[ \t]+0x([0-9A-Fa-f]+)[ \t]+0x[0-9A-Fa-f]+[ \t]+0x[0-9A-Fa-f]+[ \t]+0x([0-9A-Fa-f]+)" _ "${load_line}")
  if(NOT CMAKE_MATCH_COUNT EQUAL 2)
    continue()
  endif()

  set(VADDR_HEX "${CMAKE_MATCH_1}")
  set(MEMSZ_HEX "${CMAKE_MATCH_2}")
  math(EXPR VADDR "0x${VADDR_HEX}")
  math(EXPR MEMSZ "0x${MEMSZ_HEX}")
  math(EXPR VEND  "${VADDR} + ${MEMSZ}")

  # Segment starts in MEM1 but extends past 0x81800000.
  if(VADDR GREATER_EQUAL MEM1_START AND VADDR LESS MEM1_END AND VEND GREATER MEM1_END)
    message(FATAL_ERROR
      "MEM1 check failed: PT_LOAD crosses MEM1 end (start=0x${VADDR_HEX}, memsz=0x${MEMSZ_HEX}).\n"
      "Line: ${load_line}")
  endif()

  # Segment starts in the invalid hole between MEM1 and MEM2.
  if(VADDR GREATER_EQUAL MEM1_END AND VADDR LESS MEM2_START AND MEMSZ GREATER 0)
    message(FATAL_ERROR
      "MEM1 check failed: PT_LOAD starts in unmapped gap (start=0x${VADDR_HEX}, memsz=0x${MEMSZ_HEX}).\n"
      "Line: ${load_line}")
  endif()
endforeach()

message(STATUS "MEM1 check passed for ${ELF_FILE}")
