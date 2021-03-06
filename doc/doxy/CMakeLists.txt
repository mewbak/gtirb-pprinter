# based on sample in https://majewsky.wordpress.com/2010/08/14/tip-of-the-day-cmake-and-doxygen/
# add a target to generate API documentation with Doxygen

cmake_minimum_required(VERSION 3.3)

find_package(Doxygen)
if(DOXYGEN_FOUND)


set(DOC_INDIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ROOTDIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

set(BUILDFILES_IN preprocmd.py DoxygenLayout.xml)
foreach(_inf ${BUILDFILES_IN})
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_inf} ${CMAKE_CURRENT_BINARY_DIR}/${_inf} @ONLY)
endforeach()

# Handle version initialization
file(READ "${ROOTDIR}/version.txt" ver)

string(REGEX MATCH "VERSION_MAJOR ([0-9]*)" _ ${ver})
set(GTIRB_PPRINTER_MAJOR_VERSION ${CMAKE_MATCH_1})

string(REGEX MATCH "VERSION_MINOR ([0-9]*)" _ ${ver})
set(GTIRB_PPRINTER_MINOR_VERSION ${CMAKE_MATCH_1})

string(REGEX MATCH "VERSION_PATCH ([0-9]*)" _ ${ver})
set(GTIRB_PPRINTER_PATCH_VERSION ${CMAKE_MATCH_1})

configure_file("${ROOTDIR}/include/gtirb_pprinter/version.h.in"
  "${ROOTDIR}/include/gtirb_pprinter/version.h"
  @ONLY)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

# adapted from https://gist.github.com/abravalheri/11214134

macro(move_filename file_list newdir)
  foreach(src_file ${${file_list}})
    get_filename_component(src_file_name "${src_file}" NAME)
    list(REMOVE_ITEM ${file_list} "${src_file}")
    list(APPEND ${file_list} "${newdir}/${src_file_name}")
  endforeach()
endmacro()


# ----------------------------------------------------------------------
# copy md files into the working directory
# ----------------------------------------------------------------------


set(MDFILES_IN  "${ROOTDIR}/CONTRIBUTING.md" "${ROOTDIR}/gtirb-pprinter.md" "${ROOTDIR}/README.md")

set(MDFILES ${MDFILES_IN})
move_filename(MDFILES ${CMAKE_CURRENT_BINARY_DIR})
# message("MDFILES = ${MDFILES}")

foreach(_inmd ${MDFILES_IN})
   get_filename_component(_outmd "${_inmd}" NAME)
   add_custom_COMMAND(OUTPUT ${_outmd}
     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
     DEPENDS ${_inmd}
     COMMAND ${CMAKE_COMMAND} -E copy ${_inmd} "${_outmd}.in"
     COMMAND python preprocmd.py "${_outmd}.in" ${_outmd}
     COMMENT "processing ${_outmd}"
     VERBATIM
)
endforeach()


#----------------------------------------------------------------------
# Main target
# ----------------------------------------------------------------------

add_custom_target(doc
WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
DEPENDS ${DOTFILES}
DEPENDS ${MDFILES}
COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
COMMENT "Generating API documentation with Doxygen" VERBATIM
)
endif(DOXYGEN_FOUND)
