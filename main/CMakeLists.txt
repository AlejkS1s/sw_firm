set(COMPONENT_SRCS
    main.c
    board.c
    connection.c
    http_server.c
    ipc.c
    nvs_store.c
    routines.c
    timing.c
    power.c
    state.c
)
set(COMPONENT_REQUIRES json mdns)
register_component()
