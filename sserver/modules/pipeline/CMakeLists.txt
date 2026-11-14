# pipeline：共享采集与编码子流。
#
# 当前是最小可用的单子流版本：采集泵 → 编码泵 → 分发泵三条线程。
# 多会话共享采集、多档位子流、编码耗时统计等随 app 会话管理一起加。
add_library(sserver_pipeline STATIC
    src/StreamProducer.cpp
    src/SyntheticCapture.cpp
)
add_library(sserver::pipeline ALIAS sserver_pipeline)
target_include_directories(sserver_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_pipeline
    PUBLIC sserver::base sserver::capture sserver::encoding sserver::appconfig
    PRIVATE Threads::Threads)
sserver_set_warnings(sserver_pipeline)
