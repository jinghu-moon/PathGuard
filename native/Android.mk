LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := folder_manager
LOCAL_SRC_FILES := folder_manager.cpp rule_config.cpp path_mapper.cpp rule_engine.cpp path_kind_cache.cpp
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := folder_manager_daemon
LOCAL_SRC_FILES := folder_manager_daemon.cpp daemon_utils.cpp rule_engine.cpp path_mapper.cpp path_kind_cache.cpp
LOCAL_LDLIBS := -llog
include $(BUILD_EXECUTABLE)
