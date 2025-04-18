# Copyright 2013-present Barefoot Networks, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set (IR_SRCS
  annotations.cpp
  base.cpp
  bitrange.cpp
  dbprint.cpp
  dbprint-expression.cpp
  dbprint-stmt.cpp
  dbprint-type.cpp
  dbprint-p4.cpp
  dump.cpp
  expression.cpp
  ir.cpp
  irutils.cpp
  json_parser.cpp
  loop-visitor.cpp
  node.cpp
  pass_manager.cpp
  pass_utils.cpp
  splitter.cpp
  type.cpp
  visitor.cpp
  write_context.cpp
)

set (IR_HDRS
  annotations.h
  configuration.h
  dbprint.h
  dump.h
  id.h
  indexed_vector.h
  ir-inline.h
  ir-tree-macros.h
  ir.h
  ir-traversal.h
  ir-traversal-internal.h
  irutils.h
  json_generator.h
  json_loader.h
  json_parser.h
  namemap.h
  node.h
  nodemap.h
  pass_manager.h
  pass_utils.h
  splitter.h
  vector.h
  visitor.h
)

set (BASE_IR_DEF_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/base.def
  ${CMAKE_CURRENT_SOURCE_DIR}/type.def
  ${CMAKE_CURRENT_SOURCE_DIR}/expression.def
  ${CMAKE_CURRENT_SOURCE_DIR}/ir.def
)

set (IR_DEF_FILES ${IR_DEF_FILES} ${BASE_IR_DEF_FILES} PARENT_SCOPE)

add_library (ir STATIC ${IR_SRCS})
target_link_libraries(ir PRIVATE absl::flat_hash_map ${LIBGC_LIBRARIES})


add_dependencies(ir genIR)


