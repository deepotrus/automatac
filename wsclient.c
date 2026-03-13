#include <libwebsockets.h> //$ sudo apt install libwebsockets-dev
#include <cjson/cJSON.h> //$ sudo apt install libcjson-dev 
#include <stdio.h>
#include <time.h>

// Structure for tracking user session, like ping/pong timers, flags ...
struct my_session {
  int subscribe_sent; // initialized at 0 by default
  int ping_sent;
  time_t last_ping;
  time_t last_pong;
  char* msg_buf;  //heap-allocated, grows as needed
  size_t msg_len; // bytes accumulated so far
};

// Reconnection logic requires tracking also the state a GLOBAL context
struct app_state {
  struct lws_context *ctx;
  struct lws_client_connect_info ccinfo;
  struct lws *wsi;
  int needs_reconnect; // fail on retry connection logic, needs reconnect from main while
};

// Global ws context
struct app_state app = {0};

int callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

void handle_event(cJSON *event) {
  if (strcmp(event->valuestring, "subscribe") == 0) {
    printf("Subscribe successful\n");
  } else {
      printf("Subscribe unsuccessful: %s\n", (char *)event);
  }
}

void handle_action(cJSON *action, struct my_session *session) {
  if (strcmp(action->valuestring, "snapshot") == 0) {
      printf("Received full order book snapshot\n");
  } else if (strcmp(action->valuestring, "update") == 0) {
      printf("Received order book update\n");
  }
}

// CALLBACKS HANDLES FULL DUPLEX COMMUNICATION WITH SERVER
int callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {

  struct my_session *session = (struct my_session *)user;

  switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      printf("LWS_CALLBACK_CLIENT_ESTABLISHED\n");
      lws_callback_on_writable(wsi);
      lws_set_timer_usecs(wsi, 30*LWS_USEC_PER_SEC); // schedules a callback in the future: 30 seconds

      break;
    case LWS_CALLBACK_CLIENT_WRITEABLE: // FIRES WHENEVER I TELL HIM TO 
      printf("LWS_CALLBACK_CLIENT_WRITEABLE\n");

      if (!session->subscribe_sent) {
        char *symbol = "SOLUSDT";
        char buf[LWS_PRE + 512];
        char *msg = (char *)(buf + LWS_PRE);
        int len = snprintf(msg, 512,
            "{\"op\":\"subscribe\",\"args\":[{\"instType\":\"USDT-FUTURES\",\"channel\":\"books\",\"instId\":\"%s\"}]}",
            symbol);
        lws_write(wsi, (unsigned char *)msg, len, LWS_WRITE_TEXT);

        session->subscribe_sent = 1;
      }

      break;
    case LWS_CALLBACK_CLIENT_RECEIVE: // FIRES WHENEVER SERVER SENDS ME DATA
      printf("LWS_CALLBACK_CLIENT_RECEIVE\n");

      // handle raw "pong" before JSON parsing
      if (len == 4 && strncmp((char *)in, "pong", 4) == 0) { // using strncmp because in is not null-terminated
          printf("Pong received!\n");
          session->last_pong = time(NULL);
          break;
      }

      // before copying new data, resize the buffer to allow new data to come in
      char *tmp = realloc(session->msg_buf, session->msg_len + len);
      if (!tmp) { // realloc may fail for any reason
          free(session->msg_buf);  // no point in keeping a partial buffer
          session->msg_buf = NULL;
          session->msg_len = 0;
          break;
      }
      session->msg_buf = tmp;
      memcpy(session->msg_buf + session->msg_len, in, len);
      session->msg_len += len;

      // before parsing json, need to see if last fragment, otherwise json parse may fail
      if (!lws_is_final_fragment(wsi)) {
        printf("This is not final fragment\n");
        break;
      } else {
        printf("This is the last fragment:\n");
        printf("%.*s\n", (int)session->msg_len, session->msg_buf);
      }

      //cJSON *response = cJSON_ParseWithLength((char *)in, len);
      cJSON *response = cJSON_ParseWithLength(session->msg_buf, session->msg_len);
      if (!response) {
          printf("Failed to parse JSON\n");
          printf("Raw Print: %.*s\n", (int)len, (char *)in);
          break;
      }

      cJSON *event = cJSON_GetObjectItem(response, "event");
      cJSON *action = cJSON_GetObjectItem(response, "action");

      if (cJSON_IsString(event)) {
        handle_event(event); // events: subscribe
      } else if (cJSON_IsString(action)) {
        handle_action(action, session); // actions: snapshot, update (needs session)
      }

      cJSON_Delete(response);  // always free!

      free(session->msg_buf);
      session->msg_buf = NULL;
      session->msg_len = 0;
      break;
    case LWS_CALLBACK_TIMER:
      // check if previous ping got a pong (pong never arrived after last ping)
      if ( (session->last_ping > 0) && (session->last_pong < session->last_ping)) {
        printf("Pong timeout! Reconnecting...\n");
        //reconnect logic
        app.wsi = lws_client_connect_via_info(&app.ccinfo);

        // app.wsi may still fail (be NULL), thus a reconnection is needed
        if (!app.wsi) {
          app.needs_reconnect = 1;
        }

        return -1; // only destroys the wsi, not the entire env like lws_context_destroy
        // right after return -1 CLIENT_CLOSED is fired on old wsi, and new wsi starts its callback routine
      }

      //next ping
      unsigned char buf[LWS_PRE + 256];
      memcpy(buf + LWS_PRE, "ping", 4); // 4 should be len
      lws_write(wsi, buf + LWS_PRE, 4, LWS_WRITE_TEXT);
      printf("PING sent\n");
      session->last_ping = time(NULL);
      lws_set_timer_usecs(wsi, 30 * LWS_USEC_PER_SEC);  // reschedule for next ping
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      printf("LWS_CALLBACK_CLIENT_CONNECTION_ERROR: Initial Connection Attempt fails (DNS failure, server down, TLS error...\n");
      return -1;
      break;
    case LWS_CALLBACK_CLIENT_CLOSED:
      printf("LWS_CALLBACK_CLIENT_CLOSED\n");
      break;
    default:
      printf("REASON NOT HANDLED\n");
    break;
  }

  return 0;
}


int main(int argc, char* argv[]) {

  struct lws_protocols protocols[] = {
    { "my-protocol", callback, sizeof(struct my_session), 0},
    { NULL, NULL, 0, 0}
  };

  // settings of the lws context, fill with zero
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN; // we are client
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;


  app.ctx = lws_create_context(&info);
  if (!app.ctx) return 1;

  // settings for the client connection info
  memset(&app.ccinfo, 0, sizeof(app.ccinfo));
  app.ccinfo.context = app.ctx;
  app.ccinfo.address = "ws.bitget.com";
  app.ccinfo.port = 443;
  app.ccinfo.path = "/v2/ws/public";
  app.ccinfo.host = app.ccinfo.address;
  app.ccinfo.origin = app.ccinfo.address;
  app.ccinfo.ssl_connection = LCCSCF_USE_SSL;
  app.ccinfo.protocol = "my-protocol"; // match the protocol array

  // save in global context
  app.wsi = lws_client_connect_via_info(&app.ccinfo);
  if (!app.wsi) {
    printf("Connection Failed\n");
  } else {
    printf("Connection initiated\n");
  }

  while (lws_service(app.ctx, 0) >= 0) {
    if (app.needs_reconnect == 1) {
      printf("Attempt reconnection from main while...\n");
      sleep(5); // backoff
      app.wsi = lws_client_connect_via_info(&app.ccinfo);
      if (app.wsi) {
        app.needs_reconnect = 0;
        printf("Connection initiated from retry.\n");
      }
    }
  }

  lws_context_destroy(app.ctx);
  printf("Context ddestroyed\n");

  return 0;
}