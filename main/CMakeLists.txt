set(COMPONENT_SRCS
    main.c
    board.c
    connection.c
    countdown.c
    diagnostics.c
    http/http_server.c
    http/http_handlers_state.c
    http/http_handlers_control.c
    http/http_handlers_routines.c
    http/http_util.c
    nvs_store.c
    routines.c
    sse.c
    timing.c
    power.c
    state.c
)
set(COMPONENT_REQUIRES json mdns)
set(COMPONENT_ADD_INCLUDEDIRS . http)
register_component()
