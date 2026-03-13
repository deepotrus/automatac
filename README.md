# WebSocket Client in C — Learning Progress
This is a WebSocket client in C using libwebsockets that connects to the Bitget exchange
public API, subscribes to order book data, handles fragmented messages, maintains
the connection with application-level ping/pong, and reconnects automatically on failure.

## Build and Run
bash
$ sudo apt install libwebsockets-dev libcjson-dev
$ gcc wsclient.c -lwebsockets -lcjson -o wsclient
$ ./wsclient
## Features Implemented

### Application-level ping/pong
- Send raw "ping" string every 30 seconds via LWS_CALLBACK_TIMER
- Detect "pong" in LWS_CALLBACK_CLIENT_RECEIVE with strncmp (not null-terminated!)
- If last_pong < last_ping when timer fires → server is dead → reconnect

### Reconnection Logic
- return -1 from callback closes only the wsi, not the context
- LWS_CALLBACK_CLIENT_CLOSED fires after return -1 on the old wsi
- Global app_state holds ctx, ccinfo, wsi, needs_reconnect
- On pong timeout: call lws_client_connect_via_info(&app.ccinfo) then return -1
- If that fails (server down): set needs_reconnect = 1, retry from main loop with sleep(5) backoff
- New connection fires ESTABLISHED → subscribe automatically (fresh session, flag reset to 0)

### JSON handling
- Outgoing: snprintf for fixed schemas where only the symbol name varies
- Incoming: cJSON_ParseWithLength() — respects length, safe for non-null-terminated buffers
- Always cJSON_Delete() after use
- Use strncmp / %.*s everywhere since in is never null-terminated
- Message types are mutually exclusive — use else if to dispatch: event vs action

### Message fragmentation recomposition
- Large messages (400-level order book snapshots) arrive in multiple chunks
- Detect with lws_is_final_fragment(wsi) — accumulate until true, then parse
- Buffer stored in my_session as a heap-allocated char *msg_buf + size_t msg_len
- Use realloc to grow; on failure free the partial buffer and discard the message
- realloc pattern: always assign to a temp pointer first to avoid losing the original on failure
- Reset msg_buf = NULL and msg_len = 0 after each complete message
