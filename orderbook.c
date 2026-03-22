#include <stdint.h> //for uint64_t
#include <stdlib.h> //str to decimal
#include <cjson/cJSON.h>
#include <math.h>
#include <string.h> // for strncpy
#include <stdio.h> // for snprintf
#include <zlib.h> // for crc32 calculation

#include "orderbook.h"

void ob_apply_snapshot(struct Orderbook *ob, cJSON *data) {

  cJSON *data_buf = cJSON_GetArrayItem(data, 0);

  cJSON *bids = cJSON_GetObjectItem(data_buf, "bids");
  cJSON *asks = cJSON_GetObjectItem(data_buf, "asks");
  cJSON *ts = cJSON_GetObjectItem(data_buf, "ts");
  cJSON *seq = cJSON_GetObjectItem(data_buf, "seq");
  cJSON *checksum = cJSON_GetObjectItem(data_buf, "checksum");

  ob->last_ts = strtoull(ts->valuestring, NULL, 10);
  ob->seq = (uint64_t)seq->valuedouble;
  ob->checksum = (int32_t)checksum->valuedouble;

  int bids_len = cJSON_GetArraySize(bids);
  int asks_len = cJSON_GetArraySize(asks);

  cJSON *price_str;
  cJSON *qty_str;
  cJSON *level;

  for (int b = 0; b<bids_len; b++) {
    level = cJSON_GetArrayItem(bids, b); // level is price-quantity
    price_str = cJSON_GetArrayItem(level, 0);
    qty_str = cJSON_GetArrayItem(level, 1);

    ob->bids[b].price = strtod(price_str->valuestring, NULL);
    ob->bids[b].quantity = strtod(qty_str->valuestring, NULL);

    if (b<CRC_OB_LEVELS) {
      strncpy(ob->bids[b].price_str, price_str->valuestring, 32);
      strncpy(ob->bids[b].qty_str, qty_str->valuestring, 32);
    }
  }

  ob->count_bids = bids_len;

  for (int a = 0; a<asks_len; a++) {
    level = cJSON_GetArrayItem(asks, a); // level is price-quantity
    price_str = cJSON_GetArrayItem(level, 0);
    qty_str = cJSON_GetArrayItem(level, 1);

    ob->asks[a].price = strtod(price_str->valuestring, NULL);
    ob->asks[a].quantity = strtod(qty_str->valuestring, NULL);

    if (a<CRC_OB_LEVELS) {
      strncpy(ob->asks[a].price_str, price_str->valuestring, 32);
      strncpy(ob->asks[a].qty_str, qty_str->valuestring, 32);
    }
  }

  ob->count_asks = asks_len;
}

// helper method general for bid and ask
void remove_level(struct PriceLevel *level, int *count, int l) {
  for (int i = l; i<*count-1; i++) {
    level[i] = level[i+1]; // both fields get copied in one line.
    //level[i].price = level[i+1].price;
    //level[i].quantity = level[i+1].quantity;
  }
  (*count)--;
}

void update_level(struct PriceLevel *level, int l, double quantity) {
  level[l].quantity = quantity;
}

void add_level(struct PriceLevel *level, int *count, double price, double quantity, int bid) {
  // level l must be discovered here given if searching in bid side or ask side of the orderbook
  int l = 0;
  if (bid) {
    while(l<*count && level[l].price > price) {
      l++;
    }
  } else {
    while(l<*count && level[l].price < price) {
      l++;
    }
  }

  for (int i = (*count); i>l; i--) {
    level[i] = level[i-1];
    //ob->bids[i+1].price = ob->bids[i].price;
    //ob->bids[i+1].quantity = ob->bids[i].quantity;
  }

  level[l].price = price;
  level[l].quantity = quantity;

  (*count)++;

}

int32_t calc_ob_checksum(struct Orderbook *ob) {
  int32_t checksum;

  // min between count_bids, count_asks and CRC_OB_LEVELS, ref https://www.bitget.com/api-doc/contract/websocket/public/Order-Book-Channel
  int m = CRC_OB_LEVELS;
  if (ob->count_bids < m) {
    m = ob->count_bids;
  }
  if (ob->count_asks < m) {
    m = ob->count_asks;
  }

  char crc_buf[4096];
  int offset = 0; //keeps trak of position in crc_buf while building

  offset += snprintf(crc_buf + offset, sizeof(crc_buf) - offset, "%s:%s:%s:%s", ob->bids[0].price_str, ob->bids[0].qty_str, ob->asks[0].price_str, ob->asks[0].qty_str); // NULL terminator for cutting out remaining unused bytes from char [32]

  for (int i = 1; i < m; i++) {
    offset += snprintf(crc_buf + offset, sizeof(crc_buf) - offset, ":%s:%s:%s:%s", ob->bids[i].price_str, ob->bids[i].qty_str, ob->asks[i].price_str, ob->asks[i].qty_str); // NULL terminator for cutting out remaining unused bytes from char [32]
  }

  // calculate the signed 32 with zlib
  checksum = (int32_t)crc32(0, (const Bytef *)crc_buf, strlen(crc_buf));
  return checksum;
}

void ob_apply_update(struct Orderbook *ob, cJSON *data) {
  cJSON *data_buf = cJSON_GetArrayItem(data, 0);

  cJSON *bids = cJSON_GetObjectItem(data_buf, "bids");
  cJSON *asks = cJSON_GetObjectItem(data_buf, "asks");
  cJSON *ts = cJSON_GetObjectItem(data_buf, "ts");
  cJSON *seq = cJSON_GetObjectItem(data_buf, "seq");

  ob->last_ts = strtoull(ts->valuestring, NULL, 10);
  ob->seq = (uint64_t)seq->valuedouble;

  int bids_len = cJSON_GetArraySize(bids);
  int asks_len = cJSON_GetArraySize(asks);

  cJSON *price_str;
  cJSON *qty_str;
  cJSON *level;

  double price;
  double quantity;

  int found;

  for (int a = 0; a<asks_len; a++) {
    found = 0;
    level = cJSON_GetArrayItem(asks, a); // level is price-quantity
    price_str = cJSON_GetArrayItem(level, 0);
    qty_str = cJSON_GetArrayItem(level, 1);

    price = strtod(price_str->valuestring, NULL);
    quantity = strtod(qty_str->valuestring, NULL);

    for (int l = 0; l<ob->count_asks; l++) {
      if (fabs(ob->asks[l].price - price) < 1e-9) {
        found = 1;
        if (quantity < 1e-9) { // Rule 1: qty=0 then remove the level
          remove_level(ob->asks, &ob->count_asks, l);
        } else { // Rule 2: update existing level
          update_level(ob->asks, l, quantity);
        }
      }
    }
  
    if (!found) { // Rule 3: add new level for ask
      add_level(ob->asks, &ob->count_asks, price, quantity, 0);
    }
  }

  for (int b = 0; b<bids_len; b++) {
    found = 0;
    
    level = cJSON_GetArrayItem(bids, b); // level is price-quantity
    price_str = cJSON_GetArrayItem(level, 0);
    qty_str = cJSON_GetArrayItem(level, 1);

    price = strtod(price_str->valuestring, NULL);
    quantity = strtod(qty_str->valuestring, NULL);

    for (int l = 0; l<ob->count_bids; l++) {
      if (fabs(ob->bids[l].price - price) < 1e-9) {
        found = 1;
        if (quantity < 1e-9) { // Rule 1: qty=0 then remove the level
          remove_level(ob->bids, &ob->count_bids, l);
        } else { // Rule 2: update existing level
          update_level(ob->bids, l, quantity);
        }
      }
    }
  
    // Rule 3: add new level for bid
    if (!found) {
      add_level(ob->bids, &ob->count_bids, price, quantity, 1);
    }
  }

  // After update verify checksum
  int32_t checksum = calc_ob_checksum(ob);

}