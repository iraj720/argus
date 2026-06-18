# CMake generated Testfile for 
# Source directory: /Users/nimarafieimehr/gibical/cgo/irajstreamer3
# Build directory: /Users/nimarafieimehr/gibical/cgo/irajstreamer3/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(runtime_manifest_test "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/build/irajstreamer3_runtime_manifest_test")
set_tests_properties(runtime_manifest_test PROPERTIES  _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;175;add_test;/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;0;")
add_test(packet_adapter_test "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/build/irajstreamer3_packet_adapter_test")
set_tests_properties(packet_adapter_test PROPERTIES  _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;176;add_test;/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;0;")
add_test(model_path_test "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/build/irajstreamer3_model_path_test")
set_tests_properties(model_path_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/.." _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;177;add_test;/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;0;")
add_test(clip_tokenizer_test "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/build/irajstreamer3_clip_tokenizer_test")
set_tests_properties(clip_tokenizer_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/cgo/irajstreamer3" _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;179;add_test;/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;0;")
add_test(side_by_side_frame_test "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/build/irajstreamer3_side_by_side_frame_test")
set_tests_properties(side_by_side_frame_test PROPERTIES  WORKING_DIRECTORY "/Users/nimarafieimehr/gibical/cgo/irajstreamer3" _BACKTRACE_TRIPLES "/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;181;add_test;/Users/nimarafieimehr/gibical/cgo/irajstreamer3/CMakeLists.txt;0;")
subdirs("libdatachannel")
