add_library(SceCodecEnginePerf STATIC include/SceCodecEnginePerf/exports.h src/SceCodecEnginePerf.cpp)
target_include_directories(SceCodecEnginePerf PUBLIC include)
target_link_libraries(SceCodecEnginePerf PRIVATE module)
