# CMake generated Testfile for 
# Source directory: /Users/nimarafieimehr/gibical/argus
# Build directory: /Users/nimarafieimehr/gibical/argus/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(runtime_manifest_test "/Users/nimarafieimehr/gibical/argus/build/argus_runtime_manifest_test")
set_tests_properties(runtime_manifest_test PROPERTIES  _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;180;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
add_test(remux_muxer_test "/Users/nimarafieimehr/gibical/argus/build/argus_remux_muxer_test")
set_tests_properties(remux_muxer_test PROPERTIES  _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;181;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
add_test(packet_adapter_test "/Users/nimarafieimehr/gibical/argus/build/argus_packet_adapter_test")
set_tests_properties(packet_adapter_test PROPERTIES  _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;182;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
add_test(model_path_test "/Users/nimarafieimehr/gibical/argus/build/argus_model_path_test")
set_tests_properties(model_path_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/argus/.." _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;183;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
add_test(clip_tokenizer_test "/Users/nimarafieimehr/gibical/argus/build/argus_clip_tokenizer_test")
set_tests_properties(clip_tokenizer_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/argus" _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;185;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
add_test(side_by_side_frame_test "/Users/nimarafieimehr/gibical/argus/build/argus_side_by_side_frame_test")
set_tests_properties(side_by_side_frame_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/argus" _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;187;add_test;/Users/nimarafieimehr/gibical/argus/CMakeLists.txt;0;")
subdirs("libdatachannel")
