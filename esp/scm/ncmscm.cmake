################################################################################
#    Copyright (C) 2011 HPCC Systems.
#
#    All rights reserved. This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Affero General Public License as
#    published by the Free Software Foundation, either version 3 of the
#    License, or (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#    You should have received a copy of the GNU Affero General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.
################################################################################

#####################################################
# Description:
# ------------
#    Cmake include file ncm scm file generation
#####################################################

set ( ESPSCM_SOURCE_DIR ${HPCC_SOURCE_DIR}/esp/scm )
set ( ESPSCM_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated )
GET_TARGET_PROPERTY(HIDL_EXE hidl LOCATION)
GET_TARGET_PROPERTY(ESDL_EXE esdl LOCATION)

set ( ECMCSM_SRCS 
      ws_riskwise.ncm
      ws_riskwise_wsidentity.ncm
      wsm_riskwise1.ncm
      wsm_riskwise2.ncm
      wsm_riskwise3.ncm
    )

foreach ( loop_var ${ESPSCM_SRCS} )
    string(  REGEX REPLACE "[.]ncm" "" result ${loop_var} )
    add_custom_command ( DEPENDS hidl ${ESPSCM_SOURCE_DIR}/${loop_var}
                         OUTPUT ${ESPSCM_GENERATED_DIR}/${result}.esp ${ESPSCM_GENERATED_DIR}/${result}.hpp ${ESPSCM_GENERATED_DIR}/${result}.int ${ESPSCM_GENERATED_DIR}/${result}.ipp ${ESPSCM_GENERATED_DIR}/${result}_esp.cpp ${ESPSCM_GENERATED_DIR}/${result}_esp.ipp ${ESPSCM_GENERATED_DIR}/${result}_esp_ng.cpp ${ESPSCM_GENERATED_DIR}/${result}_esp_ng.ipp
                         COMMAND ${HIDL_EXE} ${ESPSCM_SOURCE_DIR}/${result}.ncm ${ESPSCM_GENERATED_DIR}
                       )
    add_custom_command ( DEPENDS esdl ${ESPSCM_SOURCE_DIR}/${loop_var}
                         OUTPUT ${ESPSCM_GENERATED_DIR}/${result}.xml 
                         COMMAND ${ESDL_EXE} ${ESPSCM_SOURCE_DIR}/${result}.ecm ${ESPSCM_GENERATED_DIR}
                       )
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}.esp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}.hpp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}.int PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}.ipp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}_esp.cpp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}_esp.ipp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}_esp_ng.cpp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}_esp_ng.ipp PROPERTIES ESPSCM_GENERATED_DIR TRUE)
    set_source_files_properties(${ESPSCM_GENERATED_DIR}/${result}.xml PROPERTIES ESPSCM_GENERATED_DIR TRUE)
endforeach ( loop_var ${ESPECM_SRCS} )

include_directories ( ${ESPSCM_GENERATED_DIR} )
