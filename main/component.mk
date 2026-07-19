COMPONENT_REQUIRES := mdns json
COMPONENT_SRCDIRS := . http
COMPONENT_ADD_INCLUDEDIRS := . http
COMPONENT_ADD_CFLAGS := -I$(IDF_PATH)/components/mdns/private_include
